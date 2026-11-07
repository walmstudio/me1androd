/* Virtual file system and Win32 file / profile / environment / shell-folder APIs for the recompiled game.
 *
 * Windows view                         host
 *   C:\MassEffect\...                  game_dir      (Binaries, BioGame, Movies ...; cwd = C:\MassEffect\Binaries)
 *   C:\Users\Shepard\Documents\...     docs_dir      (BioWare\Mass Effect\Config\BIO*.ini, Save)
 *   C:\...                             drive_c_dir   (AppData, Temp, ProgramData ...)
 * Path components are matched case-insensitively (Windows semantics on a case-sensitive Android/Linux FS).
 *
 * Mods (v0.9.7): ME1_MODS=dir1:dir2 lays mod folders over the game without copying anything (the first one wins):
 *   <mod>/Mass Effect (or MassEffect, or a mod with BioGame/DLC/Binaries at its top) over C:\MassEffect,
 *   <mod>/Config over Documents\BioWare\Mass Effect\Config - read-only (FILE_ATTRIBUTE_READONLY; writes, deletes and
 *   moves fail with ERROR_ACCESS_DENIED), which is how ME1Controller-style mods keep UE3 from regenerating BIO*.ini.
 * An overlay only shadows or adds regular files in directories that exist in the base tree (a mod's DLC folder stays
 * invisible without the DLC); FindFirstFile lists the merged directory.
 *
 * Settings: when the game opens BioGame\Config\Default{Engine,Game,Input}.ini for reading, the user's
 * overrides (me1cfg / Android settings screen) are applied in memory (me1cfg_patch_default). The file on disk
 * and its timestamps stay untouched, so UE3's [IniVersion] check never reports an outdated ini. */
#define _GNU_SOURCE
#include "hle.h"
#include "me1settings.h"
#include "ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <pthread.h>

#define INVALID_HANDLE 0xFFFFFFFFu
#define E_FILE_NOT_FOUND 2
#define E_PATH_NOT_FOUND 3
#define E_ACCESS_DENIED 5
#define E_INVALID_HANDLE 6
#define E_NO_MORE_FILES 18
#define E_FILE_EXISTS 80
#define E_INVALID_PARAMETER 87
#define E_INSUFFICIENT_BUFFER 122
#define E_NEGATIVE_SEEK 131
#define E_DIR_NOT_EMPTY 145
#define E_ALREADY_EXISTS 183
#define E_ENVVAR_NOT_FOUND 203
#define SETERR(e) hle_set_last_error(c, (e))

/* ---------------- configuration ---------------- */
#define USER_WIN "C:\\Users\\Shepard"
#define DOCS_WIN USER_WIN "\\Documents"
#define GAME_WIN "C:\\MassEffect"
static char g_game[1024], g_docs[1024], g_drive_c[1024];
static char g_cwd[1100] = GAME_WIN "\\Binaries";
static int g_inited, g_trace;
typedef struct { const char* win; const char* host; } Mount;
static Mount g_mounts[3];
#define MAX_OVL 8
#define CONFIG_WIN DOCS_WIN "\\BioWare\\Mass Effect\\Config"
typedef struct { const char* win; char host[1024]; int ro, files; } Ovl;
static Ovl g_ovl[MAX_OVL]; static int g_novl;
static uint64_t g_ovl_hits;
static _Thread_local int t_ro;   /* the last vfs_map hit a read-only overlay file */
static int mkdirs(const char* dir);
static void dc_flush(void);
static void set_dir(char* dst, size_t cap, const char* v);
static int is_dir(const char* p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int count_files(const char* dir, int depth) {
    DIR* d = opendir(dir); if (!d) return 0;
    int n = 0; struct dirent* de; char p[1100];
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name); struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 12) n += count_files(p, depth + 1); } else if (S_ISREG(st.st_mode)) n++;
    }
    closedir(d); return n;
}
/* case-insensitive child lookup: dir/<name> -> out (1 if found) */
static int find_ci(const char* dir, const char* name, char* out, size_t cap) {
    DIR* d = opendir(dir); if (!d) return 0;
    struct dirent* de; int ok = 0;
    while ((de = readdir(d))) if (!strcasecmp(de->d_name, name)) { snprintf(out, cap, "%s/%s", dir, de->d_name); ok = 1; break; }
    closedir(d); return ok;
}
static void ovl_add(const char* win, const char* host, int ro, const char* mod) {
    if (g_novl >= MAX_OVL) return;
    Ovl* o = &g_ovl[g_novl++]; o->win = win; snprintf(o->host, sizeof o->host, "%s", host); o->ro = ro;
    o->files = count_files(host, 0);
    fprintf(stderr, "[vfs] mod overlay %s: %s -> %s (%d files%s)\n", mod, host, win, o->files, ro ? ", read-only" : "");
    if (win != GAME_WIN) return;
    /* v0.9.8: DLC files of a mod (DLC\DLC_Vegas = Pinnacle Station, DLC\DLC_UNC = Bring Down the Sky) only replace files
     * whose folder exists in the game: report the ones that wait for a DLC (they work as soon as it is copied) */
    char md[1100], bd[1100], t[1100];
    if (!find_ci(host, "DLC", md, sizeof md)) return;
    int base = find_ci(g_game, "DLC", bd, sizeof bd);
    DIR* d = opendir(md); if (!d) return;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(t, sizeof t, "%s/%s", md, de->d_name); if (!is_dir(t)) continue;
        char b2[1100];
        if (base && find_ci(bd, de->d_name, b2, sizeof b2) && is_dir(b2)) continue;
        const char* what = !strcasecmp(de->d_name, "DLC_Vegas") ? " (Pinnacle Station)" : !strcasecmp(de->d_name, "DLC_UNC") ? " (Bring Down the Sky)" : "";
        fprintf(stderr, "[vfs] mod %s: DLC\\%s%s is not installed in the game - its %d files are used once the DLC is copied\n",
                mod, de->d_name, what, count_files(t, 0));
    }
    closedir(d);
}
static void ovl_init(void) {
    g_novl = 0;
    const char* e = getenv("ME1_MODS"); if (!e || !*e) return;
    char list[4096]; snprintf(list, sizeof list, "%s", e);
    for (char* save = NULL, *m = strtok_r(list, ":;", &save); m; m = strtok_r(NULL, ":;", &save)) {
        char d[1100], t[1100]; set_dir(d, sizeof d, m);
        if (!is_dir(d)) { fprintf(stderr, "[vfs] mod %s: not a directory, ignored\n", d); continue; }
        const char* name = strrchr(d, '/'); name = name ? name + 1 : d;
        static const char* const sub[] = { "Mass Effect", "MassEffect", "Mass Effect 1", NULL };
        int game = 0;
        for (int i = 0; sub[i] && !game; i++) { snprintf(t, sizeof t, "%s/%s", d, sub[i]); if (is_dir(t)) { ovl_add(GAME_WIN, t, 0, name); game = 1; } }
        if (!game) {
            static const char* const top[] = { "BioGame", "DLC", "Binaries", NULL };
            for (int i = 0; top[i]; i++) { snprintf(t, sizeof t, "%s/%s", d, top[i]); if (is_dir(t)) { ovl_add(GAME_WIN, d, 0, name); game = 1; break; } }
        }
        snprintf(t, sizeof t, "%s/Config", d);
        int cfg = is_dir(t);
        if (cfg) {
            ovl_add(CONFIG_WIN, t, 1, name);
            char base[1100]; snprintf(base, sizeof base, "%s/BioWare/Mass Effect/Config", g_docs); mkdirs(base);
        }
        if (!game && !cfg) fprintf(stderr, "[vfs] mod %s: no \"Mass Effect\", BioGame or Config folder, ignored\n", d);
    }
}

static void set_dir(char* dst, size_t cap, const char* v) {
    snprintf(dst, cap, "%s", v);
    size_t l = strlen(dst); while (l > 1 && dst[l - 1] == '/') dst[--l] = 0;
}
static void lite_reset(void);
void hle_fs_init(const char* game, const char* docs, const char* drive_c) {
    char t[1100];
    lite_reset();
    dc_flush();   /* re-init (tests): listings may be stale (folders copied from outside, e.g. a DLC) */
    if (!game) game = getenv("ME1_GAME");
    set_dir(g_game, sizeof g_game, game && *game ? game : ".");
    if (!docs) docs = getenv("ME1_DOCS");
    if (docs && *docs) set_dir(g_docs, sizeof g_docs, docs); else { snprintf(t, sizeof t, "%s/Documents", g_game); set_dir(g_docs, sizeof g_docs, t); }
    if (!drive_c) drive_c = getenv("ME1_DRIVE_C");
    if (drive_c && *drive_c) set_dir(g_drive_c, sizeof g_drive_c, drive_c); else { snprintf(t, sizeof t, "%s/drive_c", g_game); set_dir(g_drive_c, sizeof g_drive_c, t); }
    g_mounts[0] = (Mount){ DOCS_WIN, g_docs };    /* longest prefixes first */
    g_mounts[1] = (Mount){ GAME_WIN, g_game };
    g_mounts[2] = (Mount){ "C:", g_drive_c };
    const char* tr = getenv("ME1_VFS_TRACE"); g_trace = tr ? atoi(tr) : 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", GAME_WIN "\\Binaries");
    hle_mod_init(g_game);   /* v0.10.4: PhoneModify modifiers (registered ini keys are used by lite_patch above) */
    g_inited = 1;
    ovl_init();
}
static void ensure_init(void) { if (!g_inited) hle_fs_init(NULL, NULL, NULL); }

/* ---------------- strings: guest UTF-16 / ANSI <-> host UTF-8 ---------------- */
static void put_utf8(char* o, size_t cap, size_t* k, unsigned cp) {
    char b[4]; int n;
    if (cp < 0x80) { b[0] = cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | cp >> 6; b[1] = 0x80 | (cp & 63); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | cp >> 12; b[1] = 0x80 | (cp >> 6 & 63); b[2] = 0x80 | (cp & 63); n = 3; }
    else { b[0] = 0xF0 | cp >> 18; b[1] = 0x80 | (cp >> 12 & 63); b[2] = 0x80 | (cp >> 6 & 63); b[3] = 0x80 | (cp & 63); n = 4; }
    if (*k + n < cap) { memcpy(o + *k, b, n); *k += n; }
}
static char* gw_utf8(uint32_t p, char* o, size_t cap) {   /* NULL guest pointer -> NULL */
    if (!p) return NULL;
    size_t k = 0;
    for (uint32_t i = 0;; i++) {
        unsigned cp = rd16(p + 2 * i); if (!cp) break;
        if (cp >= 0xD800 && cp < 0xDC00) { unsigned lo = rd16(p + 2 * i + 2); if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i++; } }
        put_utf8(o, cap, &k, cp);
    }
    o[k] = 0; return o;
}
static char* ga_utf8(uint32_t p, char* o, size_t cap) {   /* ANSI (code page 1252 ~ Latin-1) */
    if (!p) return NULL;
    size_t k = 0; for (uint32_t i = 0; rd8(p + i); i++) put_utf8(o, cap, &k, rd8(p + i));
    o[k] = 0; return o;
}
static unsigned next_cp(const char** s) {
    const unsigned char* p = (const unsigned char*)*s; unsigned cp; int extra;
    if (*p < 0x80) { cp = *p; extra = 0; } else if ((*p & 0xE0) == 0xC0) { cp = *p & 31; extra = 1; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 15; extra = 2; } else { cp = *p & 7; extra = 3; }
    p++; while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 63);
    *s = (const char*)p; return cp;
}
static uint32_t units(const char* s, int wide) {   /* length in guest code units, without terminator */
    uint32_t n = 0; while (*s) { unsigned cp = next_cp(&s); n += (wide && cp >= 0x10000) ? 2 : 1; } return n;
}
static void store(const char* s, int wide, uint32_t dst, uint32_t cap) {   /* writes at most cap units incl. terminator */
    uint32_t k = 0; if (!cap) return;
    while (*s && k + 1 < cap) {
        unsigned cp = next_cp(&s);
        if (!wide) wr8(dst + k++, cp < 0x100 ? cp : '?');
        else if (cp >= 0x10000) { if (k + 2 >= cap) break; cp -= 0x10000; wr16(dst + 2 * k++, 0xD800 + (cp >> 10)); wr16(dst + 2 * k++, 0xDC00 + (cp & 1023)); }
        else wr16(dst + 2 * k++, cp);
    }
    if (wide) wr16(dst + 2 * k, 0); else wr8(dst + k, 0);
}
/* GetCurrentDirectory-style return: fits -> length without NUL, else required size with NUL */
static uint32_t put_str(const char* s, int wide, uint32_t dst, uint32_t cap) {
    uint32_t n = units(s, wide);
    if (dst && cap > n) { store(s, wide, dst, cap); return n; }
    return n + 1;
}
static char* garg(uint32_t p, int wide, char* o, size_t cap) { return wide ? gw_utf8(p, o, cap) : ga_utf8(p, o, cap); }

/* ---------------- paths ---------------- */
/* any Windows path (relative, '/', "\\?\", drive-relative) -> absolute "C:\a\b" without "." / ".." */
static void win_norm(const char* in, char* out, size_t cap) {
    char buf[2200];
    const char* p = in;
    if (!strncmp(p, "\\\\?\\", 4) || !strncmp(p, "//?/", 4)) p += 4;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        if (p[2] == '\\' || p[2] == '/') snprintf(buf, sizeof buf, "%s", p + 2);
        else snprintf(buf, sizeof buf, "%s\\%s", g_cwd + 2, p + 2);
    } else if (p[0] == '\\' || p[0] == '/') snprintf(buf, sizeof buf, "%s", p);
    else snprintf(buf, sizeof buf, "%s\\%s", g_cwd + 2, p);
    char* comp[256]; int n = 0;
    for (char* s = buf; *s;) {
        while (*s == '\\' || *s == '/') s++;
        if (!*s) break;
        char* e = s; while (*e && *e != '\\' && *e != '/') e++;
        char save = *e; *e = 0;
        if (!strcmp(s, ".")) {}
        else if (!strcmp(s, "..")) { if (n) n--; }
        else {
            size_t l = strlen(s); while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '.')) s[--l] = 0;   /* Win32 drops trailing dots/spaces */
            if (l && n < 256) comp[n++] = s;
        }
        if (!save) break;
        s = e + 1;
    }
    size_t k = snprintf(out, cap, "C:");
    if (!n) snprintf(out + k, cap - k, "\\");
    for (int i = 0; i < n && k < cap; i++) k += snprintf(out + k, cap - k, "\\%s", comp[i]);
}
static int prefix_len(const char* w, const char* pre) {
    size_t l = strlen(pre);
    return (!strncasecmp(w, pre, l) && (w[l] == '\\' || !w[l])) ? (int)l : -1;
}
static void dc_flush(void);
static int mkdirs(const char* dir) {   /* mkdir -p */
    char p[1100]; snprintf(p, sizeof p, "%s", dir); int made = 0, r = 0;
    for (char* s = p + 1; ; s++) {
        if (*s == '/' || !*s) {
            char ch = *s; *s = 0;
            if (!mkdir(p, 0775)) made = 1; else if (errno != EEXIST) { r = -1; break; }
            *s = ch; if (!ch) break;
        }
    }
    if (made) dc_flush();
    return r;
}
/* Directory listing cache for case-insensitive lookups. Android's /sdcard is a FUSE file system where every stat()
 * and readdir() is a round trip to a user-space daemon; resolving "..\BioGame\CookedPC\Foo.upk" used to cost one
 * stat per path component plus a full readdir() of CookedPC (thousands of entries) whenever the case differed or the
 * file did not exist. Each directory is now listed once and looked up through a hash table; every mutation made
 * through the VFS (create, mkdir, delete, rename, copy) drops the affected listings. ME1_VFS_CACHE=0 disables it. */
typedef struct DcDir {
    struct DcDir* next; char* path; uint32_t hp;
    int n, tabsz; char** names; uint32_t* lh; int* tab; char* blob;
} DcDir;
#define DC_BUCKETS 1024
static DcDir* g_dc[DC_BUCKETS];
static pthread_mutex_t g_dc_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_dc_on = -1;
static uint64_t g_dc_hits, g_dc_lists, g_dc_drops;
static uint32_t fnv_s(const char* s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static uint32_t fnv_lc(const char* s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)tolower((uint8_t)*s++)) * 16777619u; return h; }
static void dc_free(DcDir* d) { free(d->path); free(d->names); free(d->lh); free(d->tab); free(d->blob); free(d); }
static DcDir* dc_load(const char* path, uint32_t hp) {   /* NULL: not a directory */
    DIR* dir = opendir(*path ? path : "/"); if (!dir) return NULL;
    int cap = 64, n = 0; size_t bcap = 4096, bn = 0;
    size_t* off = malloc(cap * sizeof *off); char* blob = malloc(bcap);
    struct dirent* de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        size_t l = strlen(de->d_name) + 1;
        if (n == cap) { cap *= 2; off = realloc(off, cap * sizeof *off); }
        while (bn + l > bcap) { bcap *= 2; blob = realloc(blob, bcap); }
        memcpy(blob + bn, de->d_name, l); off[n++] = bn; bn += l;
    }
    closedir(dir);
    DcDir* d = calloc(1, sizeof *d);
    d->path = strdup(path); d->hp = hp; d->n = n; d->blob = blob;
    d->names = malloc((n ? n : 1) * sizeof *d->names); d->lh = malloc((n ? n : 1) * sizeof *d->lh);
    for (d->tabsz = 16; d->tabsz < 2 * n; d->tabsz *= 2) {}
    d->tab = malloc(d->tabsz * sizeof *d->tab); for (int i = 0; i < d->tabsz; i++) d->tab[i] = -1;
    for (int i = 0; i < n; i++) {
        d->names[i] = blob + off[i]; d->lh[i] = fnv_lc(d->names[i]);
        int k = d->lh[i] & (d->tabsz - 1); while (d->tab[k] >= 0) k = (k + 1) & (d->tabsz - 1);
        d->tab[k] = i;
    }
    free(off); g_dc_lists++;
    return d;
}
static DcDir* dc_get(const char* path) {   /* caller holds g_dc_mx */
    uint32_t hp = fnv_s(path); DcDir** b = &g_dc[hp & (DC_BUCKETS - 1)];
    for (DcDir* d = *b; d; d = d->next) if (d->hp == hp && !strcmp(d->path, path)) return d;
    DcDir* d = dc_load(path, hp); if (!d) return NULL;
    d->next = *b; *b = d; return d;
}
static const char* dc_find(DcDir* d, const char* comp) {   /* exact-case entry first, else the first case-insensitive one */
    uint32_t h = fnv_lc(comp); const char* ci = NULL;
    for (int k = h & (d->tabsz - 1); d->tab[k] >= 0; k = (k + 1) & (d->tabsz - 1)) {
        int i = d->tab[k]; if (d->lh[i] != h) continue;
        if (!strcmp(d->names[i], comp)) return d->names[i];
        if (!ci && !strcasecmp(d->names[i], comp)) ci = d->names[i];
    }
    return ci;
}
static void dc_drop(const char* path) {
    if (g_dc_on <= 0) return;
    pthread_mutex_lock(&g_dc_mx);
    uint32_t hp = fnv_s(path);
    for (DcDir** p = &g_dc[hp & (DC_BUCKETS - 1)]; *p; p = &(*p)->next)
        if ((*p)->hp == hp && !strcmp((*p)->path, path)) { DcDir* d = *p; *p = d->next; dc_free(d); g_dc_drops++; break; }
    pthread_mutex_unlock(&g_dc_mx);
}
static void dc_drop_parent(const char* host) {   /* a directory entry of host's parent was created or removed */
    char p[1024]; snprintf(p, sizeof p, "%s", host);
    char* s = strrchr(p, '/'); if (!s) return;
    *s = 0; dc_drop(p);
}
static void dc_flush(void) {   /* rename / rmdir / mkdir -p: drop everything (rare) */
    if (g_dc_on <= 0) return;
    pthread_mutex_lock(&g_dc_mx);
    for (int i = 0; i < DC_BUCKETS; i++) while (g_dc[i]) { DcDir* d = g_dc[i]; g_dc[i] = d->next; dc_free(d); g_dc_drops++; }
    pthread_mutex_unlock(&g_dc_mx);
}
void hle_fs_cache_stats(uint64_t* hits, uint64_t* lists, uint64_t* drops) { *hits = g_dc_hits; *lists = g_dc_lists; *drops = g_dc_drops; }

/* resolve the Windows path tail r ("\a\b", may be empty) below the host directory already in host[], each component
 * case-insensitively; returns 1 if the full host path exists, *parent_ok = its parent directory exists */
static int resolve(char* host, size_t cap, const char* r, int* parent_ok) {
    struct stat st; int exists, pok = 1;
    if (g_dc_on < 0) { const char* e = getenv("ME1_VFS_CACHE"); g_dc_on = !e || atoi(e) != 0; }
    if (g_dc_on) {
        pthread_mutex_lock(&g_dc_mx);
        DcDir* d = dc_get(host); exists = d != NULL;
        while (*r) {
            while (*r == '\\') r++;
            if (!*r) break;
            const char* e = strchr(r, '\\'); size_t l = e ? (size_t)(e - r) : strlen(r);
            char comp[300]; snprintf(comp, sizeof comp, "%.*s", (int)l, r);
            pok = exists;
            const char* real = d ? dc_find(d, comp) : NULL;
            size_t hl = strlen(host);
            snprintf(host + hl, cap - hl, "/%s", real ? real : comp);
            exists = real != NULL;
            r += l;
            if (exists && *r) d = dc_get(host); else d = NULL;   /* the next component needs this one's listing */
            if (exists && *r && !d) exists = 0;                   /* a file used as a directory */
        }
        g_dc_hits++;
        pthread_mutex_unlock(&g_dc_mx);
    } else {
        exists = stat(host, &st) == 0;
        while (*r) {
            while (*r == '\\') r++;
            if (!*r) break;
            const char* e = strchr(r, '\\'); size_t l = e ? (size_t)(e - r) : strlen(r);
            char comp[300]; snprintf(comp, sizeof comp, "%.*s", (int)l, r);
            pok = exists;
            size_t hl = strlen(host);
            snprintf(host + hl, cap - hl, "/%s", comp);
            if (exists && stat(host, &st) != 0) {
                exists = 0; host[hl] = 0;
                DIR* d = opendir(hl ? host : "/");
                if (d) {
                    struct dirent* de;
                    while ((de = readdir(d))) if (!strcasecmp(de->d_name, comp)) { snprintf(comp, sizeof comp, "%s", de->d_name); exists = 1; break; }
                    closedir(d);
                }
                snprintf(host + hl, cap - hl, "/%s", comp);
            }
            r += l;
        }
    }
    if (parent_ok) *parent_ok = pok;
    return exists;
}
/* mod overlays: a regular file of the first overlay that has one replaces the base path (base directories win) */
static int ovl_map(const char* w, char* host, size_t cap, int exists) {
    for (int i = 0; i < g_novl; i++) {
        int l = prefix_len(w, g_ovl[i].win); if (l < 0 || !w[l]) continue;
        char oh[1024]; snprintf(oh, sizeof oh, "%s", g_ovl[i].host);
        struct stat st;
        if (!resolve(oh, sizeof oh, w + l, NULL) || stat(oh, &st) || !S_ISREG(st.st_mode)) continue;
        if (exists && (stat(host, &st) || !S_ISREG(st.st_mode))) return exists;
        snprintf(host, cap, "%s", oh); t_ro = g_ovl[i].ro; g_ovl_hits++;
        return 1;
    }
    return exists;
}
/* normalized Windows path -> host path, resolving each component case-insensitively.
 * returns 1 if the full host path exists. *parent_ok = parent directory exists. */
static int vfs_map(const char* win, char* wout, char* host, size_t cap, int* parent_ok) {
    ensure_init();
    char w[1100]; win_norm(win, w, sizeof w);
    if (wout) snprintf(wout, 1100, "%s", w);
    const Mount* m = NULL; int ml = -1;
    for (int i = 0; i < 3; i++) { int l = prefix_len(w, g_mounts[i].win); if (l > ml) { ml = l; m = &g_mounts[i]; } }
    snprintf(host, cap, "%s", m->host);
    int pok = 1, exists = resolve(host, cap, w + ml, &pok);
    t_ro = 0;
    if (g_novl && pok) exists = ovl_map(w, host, cap, exists);
    if (parent_ok) *parent_ok = pok;
    if (g_trace > 1) fprintf(stderr, "[vfs] %s -> %s%s%s\n", w, host, exists ? "" : " (missing)", t_ro ? " (read-only mod file)" : "");
    return exists;
}
int hle_fs_host_path(const char* win, char* host, size_t cap) { return vfs_map(win, NULL, host, cap, NULL); }
static void ensure_parent_root(const char* w) {   /* mount roots exist on Windows (Documents, C:\) */
    for (int i = 0; i < 3; i++) if (prefix_len(w, g_mounts[i].win) >= 0) { mkdirs(g_mounts[i].host); return; }
}
static uint32_t win_err(int e, int parent_ok) {
    switch (e) {
    case ENOENT: return parent_ok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND;
    case ENOTDIR: return E_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: case EISDIR: return E_ACCESS_DENIED;
    case EEXIST: return E_ALREADY_EXISTS;
    case ENOTEMPTY: return E_DIR_NOT_EMPTY;
    case ENOSPC: return 112;
    case EMFILE: case ENFILE: return 4;
    default: return 31;
    }
}

/* ---------------- Default*.ini overlay ---------------- */
static int default_ini_name(const char* w, char* name) {
    static const char* const N[] = { "Engine", "Game", "Input" };
    const char* b = strrchr(w, '\\'); if (!b) return 0;
    size_t dl = b - w; if (dl < 15 || strncasecmp(b - 15, "\\BioGame\\Config", 15)) return 0;
    for (int i = 0; i < 3; i++) { char f[32]; snprintf(f, sizeof f, "Default%s.ini", N[i]); if (!strcasecmp(b + 1, f)) { strcpy(name, N[i]); return 1; } }
    return 0;
}
static uint8_t* read_all(const char* host, size_t* n) {
    FILE* f = fopen(host, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(l > 0 ? l : 1); *n = fread(b, 1, l > 0 ? l : 0, f); fclose(f); return b;
}
/* v0.10.1 «Упрощённый расчёт» (ME1_SHADER_LITE=1), engine part: cheaper lighting and effects through UE3's own switches
 * (FSystemSettings::LoadFromIni reads [SystemSettings] of BIOEngine.ini: 17 bools, 5 ints, ScreenPercentage; keys match
 * case-insensitively). Applied in memory whenever the game reads the user's BIOEngine.ini; the file on disk keeps the user's
 * values, and when the game writes BIOEngine.ini back (options menu, exit) every key still holding the lite value gets its
 * original value again (or is removed if it was absent), so switching the mode off restores everything.
 * ME1_LITE_INI=0 turns this part off, ME1_LITE_SKIP=Key1,Key2 leaves single keys alone.
 * Physics is left as is: the profiler shows PhysX at <5% of the game thread in gameplay (only level loading cooks meshes),
 * while larger time steps or no CCD would risk the Mako falling through terrain. */
typedef struct { const char *sec, *key, *val; char how; } LiteKey;   /* how: '=' set, '>' at least, '<' at most (ints) */
static const LiteKey LITE_KEYS[] = {
    {"SystemSettings", "DynamicShadows", "False", '='},            /* shadow-casting lights: extra depth + projection passes */
    {"SystemSettings", "LightEnvironmentShadows", "False", '='},   /* character/prop shadows from light environments */
    /* v0.10.2: CompositeDynamicLights removed - the game overwrites it at every map load (0x11526f21 writes 0x11b480bc),
     * so patching the ini does nothing and switching it invalidates every shader program key (FP16 / cheap sRGB) */
    {"SystemSettings", "DirectionalLightmaps", "False", '='},      /* simple lightmaps: 1 lightmap texture per pixel instead of 3 */
    {"SystemSettings", "MotionBlur", "False", '='},
    {"SystemSettings", "DepthOfField", "False", '='},              /* dialogue DOF: several full-screen blur passes */
    {"SystemSettings", "QualityBloom", "False", '='},              /* bloom stays, cheaper version */
    {"SystemSettings", "LensFlares", "False", '='},
    {"SystemSettings", "DynamicDecals", "False", '='},             /* blood / impact decals: each one re-draws geometry */
    {"SystemSettings", "Trilinear", "False", '='},                 /* bilinear mip filtering (half the texture fetches on Mali) */
    {"SystemSettings", "MaxAnisotropy", "1", '<'},
    {"SystemSettings", "SkeletalMeshLODBias", "1", '>'},           /* characters one LOD lower: less skinning and vertices */
    {"SystemSettings", "DetailMode", "1", '<'},                    /* drops primitives marked high-detail-only */
    {"WinDrv.WindowsClient", "EnableDynamicShadows", "False", '='},
};
#define NLITE (int)(sizeof LITE_KEYS / sizeof LITE_KEYS[0])
static int g_lite_ini = -1, g_lite_logged;
static struct { int had; char v[128]; } g_lite_orig[NLITE];
static char g_lite_host[1024];
static void lite_reset(void) { g_lite_ini = -1; g_lite_logged = 0; g_lite_host[0] = 0; }
static int lite_ini_on(void) {
    if (g_lite_ini < 0) {
        const char* e = getenv("ME1_SHADER_LITE"); const char* d = getenv("ME1_LITE_INI");
        g_lite_ini = e && *e == '1' && !(d && *d == '0');
    }
    return g_lite_ini;
}
static int lite_skipped(const char* key) {
    const char* e = getenv("ME1_LITE_SKIP"); if (!e) return 0;
    size_t kl = strlen(key);
    for (const char* p = e; *p; ) {
        while (*p == ',' || *p == ' ' || *p == ';') p++;
        const char* q = p; while (*q && *q != ',' && *q != ' ' && *q != ';') q++;
        if ((size_t)(q - p) == kl && !strncasecmp(p, key, kl)) return 1;
        p = q;
    }
    return 0;
}
static void trim_val(const char* v, char* o, size_t cap) {
    while (*v == ' ' || *v == '\t') v++;
    size_t l = strlen(v); while (l && (v[l - 1] == ' ' || v[l - 1] == '\t' || v[l - 1] == '\r')) l--;
    snprintf(o, cap, "%.*s", (int)l, v);
}
static int lite_same(const LiteKey* k, const char* v) {   /* value equals the lite value (bools by meaning) */
    char t[128]; trim_val(v, t, sizeof t);
    if (k->how != '=') return atoi(t) == atoi(k->val);
    if (!strcasecmp(k->val, "True") || !strcasecmp(k->val, "False")) return ini_parse_bool(t, -1) == ini_parse_bool(k->val, -1);
    return !strcasecmp(t, k->val);
}
/* the lite value for key k given the current value (NULL: absent); 0 if the current one is already as cheap */
static int lite_value(const LiteKey* k, const char* cur, char* out, size_t cap) {
    if (k->how == '=') { if (cur && lite_same(k, cur)) return 0; snprintf(out, cap, "%s", k->val); return 1; }
    int want = atoi(k->val);
    if (cur) { char t[128]; trim_val(cur, t, sizeof t); int c = atoi(t);
        if ((k->how == '>' && c >= want) || (k->how == '<' && c <= want)) return 0; }
    snprintf(out, cap, "%d", want); return 1;
}
static int lite_user_engine_ini(const char* w) { return !strcasecmp(w, CONFIG_WIN "\\BIOEngine.ini"); }
/* v0.10.4: extra keys registered from PhoneModify modifiers (hle_mod.c) - same mechanics as the lite keys, but
 * independent of ME1_SHADER_LITE: registered only when the modifiers are on, so count > 0 means active */
#define NMODKEY_MAX 48
static LiteKey g_modkeys[NMODKEY_MAX];
static unsigned g_nmodkeys;
static struct { int had; char v[128]; } g_modkey_orig[NMODKEY_MAX];
static int g_mod_logged;
void hle_fs_mod_key(const char* sec, const char* key, const char* val, char how) {
    if (g_nmodkeys >= NMODKEY_MAX || !key || !key[0]) return;
    LiteKey* k = &g_modkeys[g_nmodkeys++];
    k->sec = (sec && sec[0]) ? strdup(sec) : "SystemSettings";
    k->key = strdup(key); k->val = strdup(val && val[0] ? val : "0"); k->how = how;
}
void hle_fs_mod_keys_clear(void) { g_nmodkeys = 0; g_mod_logged = 0; }
unsigned hle_fs_mod_key_count(void) { return g_nmodkeys; }

static int lite_patch(const char* w, const char* host, uint8_t** out, size_t* outn) {
    if ((!lite_ini_on() && !g_nmodkeys) || !lite_user_engine_ini(w)) return 0;
    size_t n; uint8_t* b = read_all(host, &n); if (!b) return 0;
    Ini d; ini_load_mem(&d, b, n); free(b);
    char list[900]; size_t ll = 0; int changed = 0, modchanged = 0; list[0] = 0;
    if (lite_ini_on()) for (int i = 0; i < NLITE; i++) {
        const LiteKey* k = &LITE_KEYS[i]; char cur[128], val[64];
        int had = ini_get(&d, k->sec, k->key, cur, sizeof cur);
        g_lite_orig[i].had = had; snprintf(g_lite_orig[i].v, sizeof g_lite_orig[i].v, "%s", had ? cur : "");
        if (lite_skipped(k->key) || !lite_value(k, had ? cur : NULL, val, sizeof val)) continue;
        ini_set(&d, k->sec, k->key, val); changed++;
        if (ll + 40 < sizeof list) ll += (size_t)snprintf(list + ll, sizeof list - ll, "%s%s=%s", ll ? " " : "", k->key, val);
    }
    for (unsigned i = 0; i < g_nmodkeys; i++) {   /* PhoneModify ini keys, applied regardless of lite */
        const LiteKey* k = &g_modkeys[i]; char cur[128], val[64];
        int had = ini_get(&d, k->sec, k->key, cur, sizeof cur);
        g_modkey_orig[i].had = had; snprintf(g_modkey_orig[i].v, sizeof g_modkey_orig[i].v, "%s", had ? cur : "");
        if (!lite_value(k, had ? cur : NULL, val, sizeof val)) continue;
        ini_set(&d, k->sec, k->key, val); modchanged++;
    }
    snprintf(g_lite_host, sizeof g_lite_host, "%s", host);
    if (changed) *out = (uint8_t*)ini_serialize(&d, outn);
    else if (modchanged) *out = (uint8_t*)ini_serialize(&d, outn);
    ini_free(&d);
    if (changed && !g_lite_logged++) fprintf(stderr, "[vfs] упрощённый расчёт: BIOEngine.ini в памяти, %d параметров движка: %s (ME1_LITE_INI=0 / ME1_LITE_SKIP=...)\n",
                                  changed, changed ? list : "уже экономные");
    if (modchanged && !g_mod_logged++) fprintf(stderr, "[mod] PhoneModify: %d ini key(s) применены в памяти (BIOEngine.ini)\n", modchanged);
    return changed > 0 || modchanged > 0;
}
/* the game wrote BIOEngine.ini: keys still at their lite/mod value go back to what the user had */
static void lite_restore(const char* host) {
    if (!g_lite_host[0] || strcmp(host, g_lite_host)) return;
    Ini d; if (ini_load_file(&d, host) != 0) return;
    int n = 0;
    if (g_lite_ini > 0) for (int i = 0; i < NLITE; i++) {
        const LiteKey* k = &LITE_KEYS[i]; char cur[128];
        if (lite_skipped(k->key) || !ini_get(&d, k->sec, k->key, cur, sizeof cur) || !lite_same(k, cur)) continue;
        if (g_lite_orig[i].had) { if (lite_same(k, g_lite_orig[i].v)) continue; char t[128]; trim_val(g_lite_orig[i].v, t, sizeof t); ini_set(&d, k->sec, k->key, t); }
        else ini_remove_key(&d, k->sec, k->key);
        n++;
    }
    for (unsigned i = 0; i < g_nmodkeys; i++) {
        const LiteKey* k = &g_modkeys[i]; char cur[128];
        if (!ini_get(&d, k->sec, k->key, cur, sizeof cur) || !lite_same(k, cur)) continue;
        if (g_modkey_orig[i].had) { if (lite_same(k, g_modkey_orig[i].v)) continue; char t[128]; trim_val(g_modkey_orig[i].v, t, sizeof t); ini_set(&d, k->sec, k->key, t); }
        else ini_remove_key(&d, k->sec, k->key);
        n++;
    }
    if (n && ini_save_file(&d, host) == 0) fprintf(stderr, "[vfs] игра записала BIOEngine.ini, %d параметров возвращены к настройкам пользователя\n", n);
    ini_free(&d);
}
/* 1 + malloc'd patched contents if overrides apply */
static int patched_default(const char* w, const char* host, uint8_t** out, size_t* outn) {
    char name[16]; if (!default_ini_name(w, name)) return lite_patch(w, host, out, outn);
    size_t n; uint8_t* b = read_all(host, &n); if (!b) return 0;
    void* o; size_t on; int r = me1cfg_patch_default(g_docs, name, b, n, &o, &on);
    free(b);
    if (r) { *out = o; *outn = on; if (g_trace) fprintf(stderr, "[vfs] Default%s.ini: настройки порта применены (%zu -> %zu байт)\n", name, n, on); }
    return r;
}
static uint64_t vfs_size(const char* w, const char* host, const struct stat* st) {
    uint8_t* b; size_t n;
    if (S_ISREG(st->st_mode) && patched_default(w, host, &b, &n)) { free(b); return n; }
    return S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
}

/* ---------------- metadata helpers ---------------- */
static uint64_t ft_of(time_t s, long ns) { return ((uint64_t)s + 11644473600ull) * 10000000ull + (uint64_t)ns / 100; }
#ifdef __APPLE__
#define MT(st) ft_of((st)->st_mtimespec.tv_sec, (st)->st_mtimespec.tv_nsec)
#define AT(st) ft_of((st)->st_atimespec.tv_sec, (st)->st_atimespec.tv_nsec)
#else
#define MT(st) ft_of((st)->st_mtim.tv_sec, (st)->st_mtim.tv_nsec)
#define AT(st) ft_of((st)->st_atim.tv_sec, (st)->st_atim.tv_nsec)
#endif
static uint32_t attrs_of(const struct stat* st) {
    uint32_t a = S_ISDIR(st->st_mode) ? 0x10u : 0x20u;   /* DIRECTORY / ARCHIVE */
    if (!(st->st_mode & 0222)) a |= 1u;                  /* READONLY */
    return a;
}

/* ---------------- handles ---------------- */
#define FH_BASE 0x1000u
#define FH_MAX 1024
typedef struct { int used, fd, writable, islog, logbol, ro, isbik; uint8_t* mem; uint64_t memsz, pos; struct stat st; char host[1024]; } FHandle;
/* heartbeat stats (hle_fs_brief) and engine-log echo: writes to Documents\BioWare\Mass Effect\Logs\*.log are copied to
 * stderr as "[log] ..." lines (ME1_ECHO_LOG=0 turns this off) */
static uint64_t g_fs_opens, g_fs_rbytes, g_fs_reads, g_fs_fails, g_fs_open_ns; static char g_fs_last[256]; static int g_echo_log = -1;
uint64_t hle_fs_read_bytes(void);
uint64_t hle_fs_read_bytes(void) { return __atomic_load_n(&g_fs_rbytes, __ATOMIC_RELAXED); }   /* v1.2.1: GEARMASS spots loading screens */
static int g_fs_bik;   /* v1.2.2: Bink movies open (GEARMASS class "video") */
int hle_fs_video_open(void) { return __atomic_load_n(&g_fs_bik, __ATOMIC_RELAXED); }
const char* hle_fs_docs(void) { return g_docs[0] ? g_docs : NULL; }   /* v1.2.2: me1_gearmass_last.txt */
void hle_fs_brief(char* o, size_t n) {
    snprintf(o, n, "fs: %llu opens (%llu failed, %.1f s in CreateFile), %llu reads, %.1f MB read, %llu dirs listed; last opened: %s",
             (unsigned long long)g_fs_opens, (unsigned long long)g_fs_fails, g_fs_open_ns / 1e9, (unsigned long long)g_fs_reads,
             g_fs_rbytes / 1048576.0, (unsigned long long)g_dc_lists, g_fs_last[0] ? g_fs_last : "-");
    size_t k = strlen(o);
    if (g_novl && k + 40 < n) snprintf(o + k, n - k, "; mod files used %llu times", (unsigned long long)g_ovl_hits);
}
static void echo_log(FHandle* f, const uint8_t* p, uint32_t n) {
    char line[1024]; size_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (f->logbol) { k = (size_t)snprintf(line, sizeof line, "[log] "); f->logbol = 0; }
        if (p[i] == '\n') { line[k] = 0; fprintf(stderr, "%s\n", line); k = 0; f->logbol = 1; continue; }
        if (p[i] != '\r' && p[i] && k < sizeof line - 2) line[k++] = (char)p[i];
    }
    if (k && !f->logbol) { line[k] = 0; fprintf(stderr, "%s\n", line); f->logbol = 1; }   /* partial line: printed, continues on a new [log] line */
}
static FHandle g_fh[FH_MAX];
static FHandle* fh_get(uint32_t h) {
    if (h < FH_BASE || (h - FH_BASE) % 4) return NULL;
    uint32_t k = (h - FH_BASE) / 4;
    return k < FH_MAX && g_fh[k].used ? &g_fh[k] : NULL;
}
static uint32_t fh_new(FHandle** out) {
    for (int k = 1; k < FH_MAX; k++) if (!g_fh[k].used) { memset(&g_fh[k], 0, sizeof g_fh[k]); g_fh[k].used = 1; g_fh[k].fd = -1; *out = &g_fh[k]; return FH_BASE + 4 * k; }
    return 0;
}
static int64_t fh_size(FHandle* f) { if (f->mem) return f->memsz; struct stat st; return fstat(f->fd, &st) ? 0 : st.st_size; }
static int64_t fh_tell(FHandle* f) { return f->mem ? (int64_t)f->pos : lseek(f->fd, 0, SEEK_CUR); }

static uint32_t do_create(CPU* c, const char* path, uint32_t access, uint32_t disp, uint32_t flags, const char* api) {
    char w[1100], host[1024]; int pok = 1;
    if (!path) { SETERR(E_PATH_NOT_FOUND); return INVALID_HANDLE; }
    if (!strcasecmp(path, "CONOUT$")) { SETERR(0); return 0x10Bu; }
    if (!strcasecmp(path, "CONIN$")) { SETERR(0); return 0x10Au; }
    int ex = vfs_map(path, w, host, sizeof host, &pok), ro = t_ro;
    int wr = (access & (0x40000000u | 0x10000000u | 0x2u | 0x4u)) != 0;   /* GENERIC_WRITE / GENERIC_ALL / FILE_WRITE_DATA / FILE_APPEND_DATA */
    int rd = (access & (0x80000000u | 0x10000000u | 0x1u)) != 0 || !wr;
    if (ro && (wr || disp == 2 || disp == 5)) { SETERR(E_ACCESS_DENIED); goto fail; }   /* read-only mod file */
    struct stat st; if (ex && stat(host, &st)) ex = 0;
    int of = 0; uint32_t okerr = 0;
    switch (disp) {
    case 1: if (ex) { SETERR(E_FILE_EXISTS); goto fail; } of = O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: of = O_CREAT | O_TRUNC; if (ex) okerr = E_ALREADY_EXISTS; break;              /* CREATE_ALWAYS */
    case 3: if (!ex) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); goto fail; } break;   /* OPEN_EXISTING */
    case 4: of = O_CREAT; if (ex) okerr = E_ALREADY_EXISTS; break;                        /* OPEN_ALWAYS */
    case 5: if (!ex) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); goto fail; } of = O_TRUNC; break;   /* TRUNCATE_EXISTING */
    default: SETERR(E_INVALID_PARAMETER); goto fail;
    }
    if (ex && S_ISDIR(st.st_mode)) {
        if (!(flags & 0x02000000u) || (of & O_TRUNC)) { SETERR(E_ACCESS_DENIED); goto fail; }   /* FILE_FLAG_BACKUP_SEMANTICS */
        of = 0; wr = 0;
    }
    FHandle* f; uint32_t h = fh_new(&f);
    if (!h) { SETERR(4); goto fail; }
    snprintf(f->host, sizeof f->host, "%s", host); f->ro = ro;
    uint8_t* mem; size_t memn;
    if (ex && !wr && S_ISREG(st.st_mode) && patched_default(w, host, &mem, &memn)) {
        f->mem = mem; f->memsz = memn; f->st = st;
    } else {
        if (of & O_CREAT) ensure_parent_root(w);
        int mode = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
        f->fd = open(host, mode | of | O_CLOEXEC, 0664);
        if (f->fd < 0 && wr && rd && errno == EACCES) f->fd = open(host, O_WRONLY | of | O_CLOEXEC, 0664);
        if (f->fd < 0) { int e = errno; f->used = 0; SETERR(win_err(e, pok)); goto fail; }
        fstat(f->fd, &f->st);
        f->writable = wr;
        if (!ex && (of & O_CREAT)) dc_drop_parent(host);
        if (flags & 0x04000000u) { unlink(host); dc_drop_parent(host); }   /* FILE_FLAG_DELETE_ON_CLOSE: Unix semantics are close enough */
    }
    SETERR(okerr);
    { size_t L = strlen(w); if (!wr && L > 4 && !strcasecmp(w + L - 4, ".bik")) { f->isbik = 1; __atomic_add_fetch(&g_fs_bik, 1, __ATOMIC_RELAXED); } }
    g_fs_opens++; snprintf(g_fs_last, sizeof g_fs_last, "%s", w);
    if (g_echo_log < 0) { const char* e = getenv("ME1_ECHO_LOG"); g_echo_log = !e || atoi(e) != 0; }
    if (wr && g_echo_log) { size_t L = strlen(host); f->islog = L > 4 && !strcasecmp(host + L - 4, ".log") && strstr(host, "/Logs/") != NULL; f->logbol = 1; }
    if (g_trace) fprintf(stderr, "[vfs] %s(%s, %s) -> %08x  [%s]\n", api, w, wr ? "rw" : "r", h, host);
    return h;
fail:
    if (g_trace) fprintf(stderr, "[vfs] %s(%s) -> INVALID, error %u  [%s]\n", api, w, rd32(c->fs_base + 0x34), host);
    return INVALID_HANDLE;
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
static uint32_t timed_create(CPU* c, const char* path, uint32_t access, uint32_t disp, uint32_t flags, const char* api) {
    uint64_t t0 = now_ns(); uint32_t h = do_create(c, path, access, disp, flags, api);
    g_fs_open_ns += now_ns() - t0; if (h == INVALID_HANDLE) g_fs_fails++;
    return h;
}
static void k_CreateFileW(CPU* c) { char p[1100]; RET(timed_create(c, gw_utf8(ARG(0), p, sizeof p), ARG(1), ARG(4), ARG(5), "CreateFileW")); }
static void k_CreateFileA(CPU* c) { char p[1100]; RET(timed_create(c, ga_utf8(ARG(0), p, sizeof p), ARG(1), ARG(4), ARG(5), "CreateFileA")); }

static void k_ReadFile(CPU* c) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pread_ = ARG(3);
    if (h == 0x10Au) { if (pread_) wr32(pread_, 0); RET(1); return; }
    FHandle* f = fh_get(h);
    if (!f) { if (pread_) wr32(pread_, 0); SETERR(E_INVALID_HANDLE); RET(0); return; }
    uint64_t got = 0;
    if (f->mem) { uint64_t left = f->pos < f->memsz ? f->memsz - f->pos : 0; got = n < left ? n : left; memcpy(M + buf, f->mem + f->pos, got); f->pos += got; }
    else while (got < n) {
        ssize_t r = read(f->fd, M + buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { if (pread_) wr32(pread_, (uint32_t)got); SETERR(win_err(errno, 1)); RET(0); return; }
        if (!r) break;
        got += r;
    }
    if (pread_) wr32(pread_, (uint32_t)got);
    g_fs_reads++; g_fs_rbytes += got;
    RET(1);
}
static void k_WriteFile(CPU* c) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (h == 0x10Bu || h == 0x10Cu) { fwrite(M + buf, 1, n, stderr); if (pw) wr32(pw, n); RET(1); return; }
    FHandle* f = fh_get(h);
    if (!f) { if (pw) wr32(pw, 0); SETERR(E_INVALID_HANDLE); RET(0); return; }
    if (f->mem || !f->writable) { if (pw) wr32(pw, 0); SETERR(E_ACCESS_DENIED); RET(0); return; }
    uint64_t put = 0;
    while (put < n) {
        ssize_t r = write(f->fd, M + buf + put, n - put);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (pw) wr32(pw, (uint32_t)put); SETERR(win_err(errno, 1)); RET(0); return; }
        put += r;
    }
    if (f->islog) echo_log(f, M + buf, n);
    if (pw) wr32(pw, n);
    RET(1);
}
static int seek_to(CPU* c, FHandle* f, int64_t off, uint32_t method, int64_t* np) {
    int64_t base = method == 0 ? 0 : method == 1 ? fh_tell(f) : fh_size(f);
    if (method > 2) { SETERR(E_INVALID_PARAMETER); return 0; }
    int64_t p = base + off;
    if (p < 0) { SETERR(E_NEGATIVE_SEEK); return 0; }
    if (f->mem) f->pos = p; else if (lseek(f->fd, p, SEEK_SET) < 0) { SETERR(win_err(errno, 1)); return 0; }
    *np = p; return 1;
}
static void k_SetFilePointer(CPU* c) {
    FHandle* f = fh_get(ARG(0)); uint32_t phi = ARG(2);
    if (!f) { SETERR(E_INVALID_HANDLE); RET(0xFFFFFFFFu); return; }
    int64_t off = phi ? (int64_t)(((uint64_t)rd32(phi) << 32) | ARG(1)) : (int64_t)(int32_t)ARG(1), np;
    if (!seek_to(c, f, off, ARG(3), &np)) { RET(0xFFFFFFFFu); return; }
    if (phi) wr32(phi, (uint32_t)((uint64_t)np >> 32));
    SETERR(0); RET((uint32_t)np);
}
static void k_SetFilePointerEx(CPU* c) {   /* (h, LARGE_INTEGER dist [2 slots], PLARGE_INTEGER newpos, method) */
    FHandle* f = fh_get(ARG(0)); int64_t np;
    if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    if (!seek_to(c, f, (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1)), ARG(4), &np)) { RET(0); return; }
    if (ARG(3)) wr64(ARG(3), (uint64_t)np);
    RET(1);
}
static void k_GetFileSize(CPU* c) {
    FHandle* f = fh_get(ARG(0)); if (!f) { SETERR(E_INVALID_HANDLE); RET(0xFFFFFFFFu); return; }
    uint64_t s = fh_size(f); if (ARG(1)) wr32(ARG(1), (uint32_t)(s >> 32));
    SETERR(0); RET((uint32_t)s);
}
static void k_GetFileSizeEx(CPU* c) {
    FHandle* f = fh_get(ARG(0)); if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    wr64(ARG(1), fh_size(f)); RET(1);
}
static void k_SetEndOfFile(CPU* c) {
    FHandle* f = fh_get(ARG(0)); if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    if (f->mem || !f->writable) { SETERR(E_ACCESS_DENIED); RET(0); return; }
    RET(ftruncate(f->fd, fh_tell(f)) == 0);
}
static void k_FlushFileBuffers(CPU* c) { FHandle* f = fh_get(ARG(0)); RET(f || ARG(0) == 0x10Bu || ARG(0) == 0x10Cu); if (!f) SETERR(E_INVALID_HANDLE); }
static void stat_of(FHandle* f, struct stat* st) { if (f->mem || fstat(f->fd, st)) *st = f->st; }
static void k_GetFileTime(CPU* c) {
    FHandle* f = fh_get(ARG(0)); if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    struct stat st; stat_of(f, &st);
    if (ARG(1)) wr64(ARG(1), MT(&st));
    if (ARG(2)) wr64(ARG(2), AT(&st));
    if (ARG(3)) wr64(ARG(3), MT(&st));
    RET(1);
}
static void k_SetFileTime(CPU* c) {
    FHandle* f = fh_get(ARG(0)); if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    if (ARG(3) && f->fd >= 0) {
        uint64_t ft = rd64(ARG(3)) - 116444736000000000ull;
        struct timespec ts[2] = { { 0, UTIME_OMIT }, { (time_t)(ft / 10000000ull), (long)(ft % 10000000ull) * 100 } };
        futimens(f->fd, ts);
    }
    RET(1);
}
static void k_GetFileInformationByHandle(CPU* c) {
    FHandle* f = fh_get(ARG(0)); uint32_t p = ARG(1);
    if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    struct stat st; stat_of(f, &st); uint64_t sz = fh_size(f);
    memset(M + p, 0, 52);
    wr32(p, attrs_of(&st) | (f->ro ? 1u : 0u)); wr64(p + 4, MT(&st)); wr64(p + 12, AT(&st)); wr64(p + 20, MT(&st));
    wr32(p + 28, 0x4D453130u /* volume serial "ME10" */); wr32(p + 32, (uint32_t)(sz >> 32)); wr32(p + 36, (uint32_t)sz);
    wr32(p + 40, (uint32_t)st.st_nlink); wr32(p + 44, (uint32_t)((uint64_t)st.st_ino >> 32)); wr32(p + 48, (uint32_t)st.st_ino);
    RET(1);
}
static void k_CloseHandle(CPU* c) {
    if (hle_kobj_close(ARG(0))) { RET(1); return; }   /* events, mutexes, semaphores, threads (hle_thread.c) */
    FHandle* f = fh_get(ARG(0));
    if (f) { if (f->isbik) __atomic_sub_fetch(&g_fs_bik, 1, __ATOMIC_RELAXED);
             if (f->fd >= 0) close(f->fd); free(f->mem); f->used = 0; if (f->writable && g_lite_host[0]) lite_restore(f->host); }
    RET(1);   /* events, mutexes, processes: nothing to release */
}
static void k_Ret1(CPU* c) { RET(1); }

/* ---------------- attributes, directories, delete/move/copy ---------------- */
static void k_GetFileAttributes_(CPU* c, int wide) {
    char p[1100], w[1100], host[1024]; int pok = 1;
    if (!garg(ARG(0), wide, p, sizeof p)) { SETERR(E_PATH_NOT_FOUND); RET(0xFFFFFFFFu); return; }
    struct stat st;
    if (!vfs_map(p, w, host, sizeof host, &pok) || stat(host, &st)) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0xFFFFFFFFu); return; }
    RET(attrs_of(&st) | (t_ro ? 1u : 0u));
}
static void k_GetFileAttributesW(CPU* c) { k_GetFileAttributes_(c, 1); }
static void k_GetFileAttributesA(CPU* c) { k_GetFileAttributes_(c, 0); }
static void k_GetFileAttributesEx_(CPU* c, int wide) {   /* WIN32_FILE_ATTRIBUTE_DATA */
    char p[1100], w[1100], host[1024]; int pok = 1; uint32_t d = ARG(2);
    if (!garg(ARG(0), wide, p, sizeof p)) { SETERR(E_PATH_NOT_FOUND); RET(0); return; }
    struct stat st;
    if (!vfs_map(p, w, host, sizeof host, &pok) || stat(host, &st)) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0); return; }
    int ro = t_ro; uint64_t sz = vfs_size(w, host, &st);
    wr32(d, attrs_of(&st) | (ro ? 1u : 0u)); wr64(d + 4, MT(&st)); wr64(d + 12, AT(&st)); wr64(d + 20, MT(&st));
    wr32(d + 28, (uint32_t)(sz >> 32)); wr32(d + 32, (uint32_t)sz);
    RET(1);
}
static void k_GetFileAttributesExW(CPU* c) { k_GetFileAttributesEx_(c, 1); }
static void k_GetFileAttributesExA(CPU* c) { k_GetFileAttributesEx_(c, 0); }
static void k_SetFileAttributes_(CPU* c, int wide) {
    char p[1100], w[1100], host[1024]; int pok = 1; struct stat st;
    if (!garg(ARG(0), wide, p, sizeof p) || !vfs_map(p, w, host, sizeof host, &pok) || stat(host, &st)) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0); return; }
    if (t_ro) { RET(1); return; }   /* read-only mod file: stays read-only */
    mode_t m = (ARG(1) & 1u) ? (st.st_mode & ~0222) : (st.st_mode | 0200);
    if ((m & 07777) != (st.st_mode & 07777)) chmod(host, m & 07777);   /* best effort (FAT/sdcardfs ignore it) */
    RET(1);
}
static void k_SetFileAttributesW(CPU* c) { k_SetFileAttributes_(c, 1); }
static void k_SetFileAttributesA(CPU* c) { k_SetFileAttributes_(c, 0); }
static void k_CreateDirectory_(CPU* c, int wide) {
    char p[1100], w[1100], host[1024]; int pok = 1;
    if (!garg(ARG(0), wide, p, sizeof p)) { SETERR(E_PATH_NOT_FOUND); RET(0); return; }
    if (vfs_map(p, w, host, sizeof host, &pok)) { SETERR(E_ALREADY_EXISTS); RET(0); return; }
    ensure_parent_root(w);
    if (mkdir(host, 0775)) { SETERR(win_err(errno, pok)); RET(0); return; }
    dc_drop_parent(host);
    if (g_trace) fprintf(stderr, "[vfs] CreateDirectory(%s)  [%s]\n", w, host);
    RET(1);
}
static void k_CreateDirectoryW(CPU* c) { k_CreateDirectory_(c, 1); }
static void k_CreateDirectoryA(CPU* c) { k_CreateDirectory_(c, 0); }
static void k_RemoveDirectory_(CPU* c, int wide) {
    char p[1100], w[1100], host[1024]; int pok = 1;
    if (!garg(ARG(0), wide, p, sizeof p) || !vfs_map(p, w, host, sizeof host, &pok)) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0); return; }
    if (rmdir(host)) { SETERR(errno == ENOTDIR ? 267 /* ERROR_DIRECTORY */ : win_err(errno, 1)); RET(0); return; }
    dc_flush();
    RET(1);
}
static void k_RemoveDirectoryW(CPU* c) { k_RemoveDirectory_(c, 1); }
static void k_RemoveDirectoryA(CPU* c) { k_RemoveDirectory_(c, 0); }
static void k_DeleteFile_(CPU* c, int wide) {
    char p[1100], w[1100], host[1024]; int pok = 1; struct stat st;
    if (!garg(ARG(0), wide, p, sizeof p) || !vfs_map(p, w, host, sizeof host, &pok) || stat(host, &st)) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0); return; }
    if (t_ro) { SETERR(E_ACCESS_DENIED); RET(0); return; }
    if (S_ISDIR(st.st_mode) || unlink(host)) { SETERR(S_ISDIR(st.st_mode) ? E_ACCESS_DENIED : win_err(errno, 1)); RET(0); return; }
    dc_drop_parent(host);
    if (g_trace) fprintf(stderr, "[vfs] DeleteFile(%s)\n", w);
    RET(1);
}
static void k_DeleteFileW(CPU* c) { k_DeleteFile_(c, 1); }
static void k_DeleteFileA(CPU* c) { k_DeleteFile_(c, 0); }
static int copy_file(const char* src, const char* dst) {
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    struct stat st; fstat(in, &st);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664); if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[65536]; ssize_t r; int ok = 1;
    while ((r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, r) != r) { ok = 0; break; }
    if (r < 0) ok = 0;
    struct timespec ts[2] = { st.st_atim, st.st_mtim }; futimens(out, ts);   /* CopyFile keeps the last-write time */
    close(in); if (close(out)) ok = 0;
    return ok ? 0 : -1;
}
static void move_copy(CPU* c, int wide, int copy, int replace) {
    char a[1100], b[1100], wa[1100], wb[1100], ha[1024], hb[1024]; int pa = 1, pb = 1;
    if (!garg(ARG(0), wide, a, sizeof a) || !garg(ARG(1), wide, b, sizeof b)) { SETERR(E_INVALID_PARAMETER); RET(0); return; }
    if (!vfs_map(a, wa, ha, sizeof ha, &pa)) { SETERR(pa ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0); return; }
    int aro = t_ro, bex = vfs_map(b, wb, hb, sizeof hb, &pb);
    if (bex && !replace) { SETERR(copy ? E_FILE_EXISTS : E_ALREADY_EXISTS); RET(0); return; }
    if ((aro && !copy) || (bex && t_ro)) { SETERR(E_ACCESS_DENIED); RET(0); return; }   /* read-only mod files */
    if (!pb) { SETERR(E_PATH_NOT_FOUND); RET(0); return; }
    int r;
    if (copy) r = copy_file(ha, hb);
    else { r = rename(ha, hb); if (r && errno == EXDEV && !(r = copy_file(ha, hb))) unlink(ha); }
    dc_flush();
    if (r) { SETERR(win_err(errno, 1)); RET(0); return; }
    if (g_trace) fprintf(stderr, "[vfs] %s(%s -> %s)\n", copy ? "CopyFile" : "MoveFile", wa, wb);
    RET(1);
}
static void k_MoveFileW(CPU* c) { move_copy(c, 1, 0, 0); }
static void k_MoveFileA(CPU* c) { move_copy(c, 0, 0, 0); }
static void k_MoveFileExW(CPU* c) { move_copy(c, 1, 0, ARG(2) & 1u); }   /* MOVEFILE_REPLACE_EXISTING; cross-device moves always copy */
static void k_MoveFileExA(CPU* c) { move_copy(c, 0, 0, ARG(2) & 1u); }
static void k_CopyFileW(CPU* c) { move_copy(c, 1, 1, !ARG(2)); }
static void k_CopyFileA(CPU* c) { move_copy(c, 0, 1, !ARG(2)); }

/* ---------------- FindFirstFile / FindNextFile ---------------- */
#define FIND_BASE 0x6000u
#define FIND_MAX 256
typedef struct { int used, n, i, wide; char** names; char** ohost; uint8_t* oro; char dir[1024], wdir[1100]; } FindH;   /* ohost[i]: overlay file */
static FindH g_find[FIND_MAX];
static int wild(const char* p, const char* s) {   /* Win32 wildcards, case-insensitive */
    for (; *p; p++, s++) {
        if (*p == '*') { while (p[1] == '*') p++; if (!p[1]) return 1; for (; *s; s++) if (wild(p + 1, s)) return 1; return wild(p + 1, s); }
        if (!*s) return !strcmp(p, ".") || (p[0] == '.' && p[1] == '*' && !p[2]);   /* "name.*" also matches "name" */
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}
static int name_cmp(const void* a, const void* b) { return strcasecmp(*(char* const*)a, *(char* const*)b); }
static void fill_find(FindH* f, uint32_t d) {
    const char* name = f->names[f->i]; char host[2100], w[2200]; struct stat st;
    if (f->ohost && f->ohost[f->i]) snprintf(host, sizeof host, "%s", f->ohost[f->i]); else snprintf(host, sizeof host, "%s/%s", f->dir, name);
    snprintf(w, sizeof w, "%s\\%s", f->wdir, name);
    if (stat(host, &st)) memset(&st, 0, sizeof st);
    uint64_t sz = vfs_size(w, host, &st);
    memset(M + d, 0, f->wide ? 592 : 320);
    wr32(d, attrs_of(&st) | (f->oro && f->oro[f->i] ? 1u : 0u)); wr64(d + 4, MT(&st)); wr64(d + 12, AT(&st)); wr64(d + 20, MT(&st));
    wr32(d + 28, (uint32_t)(sz >> 32)); wr32(d + 32, (uint32_t)sz);
    store(name, f->wide, d + 44, 260);
}
static void k_FindFirstFile_(CPU* c, int wide) {
    char p[1100], w[1100], host[1024], pat[300]; int pok = 1;
    if (!garg(ARG(0), wide, p, sizeof p)) { SETERR(E_PATH_NOT_FOUND); RET(INVALID_HANDLE); return; }
    ensure_init(); win_norm(p, w, sizeof w);
    size_t pl = strlen(p);
    if (pl && (p[pl - 1] == '\\' || p[pl - 1] == '/')) { SETERR(E_FILE_NOT_FOUND); RET(INVALID_HANDLE); return; }
    char* sl = strrchr(w, '\\'); snprintf(pat, sizeof pat, "%s", sl + 1);
    if (sl == w + 2) sl[1] = 0; else *sl = 0;   /* keep "C:\" */
    if (!vfs_map(w, NULL, host, sizeof host, &pok)) { SETERR(E_PATH_NOT_FOUND); RET(INVALID_HANDLE); return; }
    int k = 1; while (k < FIND_MAX && g_find[k].used) k++;
    if (k == FIND_MAX) { SETERR(4); RET(INVALID_HANDLE); return; }
    FindH* f = &g_find[k]; memset(f, 0, sizeof *f);
    snprintf(f->dir, sizeof f->dir, "%s", host); snprintf(f->wdir, sizeof f->wdir, "%s", w);
    if (!strcmp(f->wdir + 2, "\\")) f->wdir[2] = 0;
    if (!strcmp(pat, "*.*")) strcpy(pat, "*");
    int cap = 64; f->names = malloc(cap * sizeof(char*)); f->wide = wide;
    DIR* d = opendir(host);
    if (d) {
        struct dirent* de;
        while ((de = readdir(d))) {
            if (!wild(pat, de->d_name)) continue;
            if (f->n == cap) { cap *= 2; f->names = realloc(f->names, cap * sizeof(char*)); }
            f->names[f->n++] = strdup(de->d_name);
        }
        closedir(d);
    }
    int base_n = f->n;
    for (int o = 0; o < g_novl; o++) {   /* mod overlays: their regular files in this directory */
        int l = prefix_len(f->wdir, g_ovl[o].win); if (l < 0) continue;
        char oh[1024]; snprintf(oh, sizeof oh, "%s", g_ovl[o].host);
        if (!resolve(oh, sizeof oh, f->wdir + l, NULL)) continue;
        DIR* od = opendir(oh); if (!od) continue;
        struct dirent* de;
        while ((de = readdir(od))) {
            if (!wild(pat, de->d_name)) continue;
            char fp[2100]; struct stat st; snprintf(fp, sizeof fp, "%s/%s", oh, de->d_name);
            if (stat(fp, &st) || !S_ISREG(st.st_mode)) continue;
            int j = 0; while (j < f->n && strcasecmp(f->names[j], de->d_name)) j++;
            if (!f->ohost) { f->ohost = calloc(cap, sizeof(char*)); f->oro = calloc(cap, 1); }
            if (j < f->n) {   /* shadows a base file (not a directory), unless an earlier overlay already does */
                if (f->ohost[j]) continue;
                char bp[2100]; struct stat bs; snprintf(bp, sizeof bp, "%s/%s", f->dir, f->names[j]);
                if (j < base_n && (stat(bp, &bs) || !S_ISREG(bs.st_mode))) continue;
            } else {
                if (f->n == cap) {
                    cap *= 2; f->names = realloc(f->names, cap * sizeof(char*));
                    f->ohost = realloc(f->ohost, cap * sizeof(char*)); f->oro = realloc(f->oro, cap);
                    memset(f->ohost + f->n, 0, (cap - f->n) * sizeof(char*)); memset(f->oro + f->n, 0, cap - f->n);
                }
                f->names[f->n++] = strdup(de->d_name);
            }
            f->ohost[j] = strdup(fp); f->oro[j] = (uint8_t)g_ovl[o].ro;
        }
        closedir(od);
    }
    if (!f->n) { free(f->names); free(f->ohost); free(f->oro); SETERR(E_FILE_NOT_FOUND); RET(INVALID_HANDLE); return; }
    if (f->ohost) {   /* sort names with their overlay paths */
        typedef struct { char* n; char* h; uint8_t r; } E;
        E* es = malloc(f->n * sizeof *es);
        for (int i = 0; i < f->n; i++) { es[i].n = f->names[i]; es[i].h = f->ohost[i]; es[i].r = f->oro[i]; }
        qsort(es, f->n, sizeof *es, name_cmp);   /* the name is the first member */
        for (int i = 0; i < f->n; i++) { f->names[i] = es[i].n; f->ohost[i] = es[i].h; f->oro[i] = es[i].r; }
        free(es);
    } else qsort(f->names, f->n, sizeof(char*), name_cmp);
    f->used = 1; fill_find(f, ARG(1));
    if (g_trace) fprintf(stderr, "[vfs] FindFirstFile(%s\\%s) -> %d entries\n", f->wdir, pat, f->n);
    RET(FIND_BASE + 4 * k);
}
static FindH* find_get(uint32_t h) {
    uint32_t k = (h - FIND_BASE) / 4;
    return (h >= FIND_BASE && (h - FIND_BASE) % 4 == 0 && k < FIND_MAX && g_find[k].used) ? &g_find[k] : NULL;
}
static void k_FindFirstFileW(CPU* c) { k_FindFirstFile_(c, 1); }
static void k_FindFirstFileA(CPU* c) { k_FindFirstFile_(c, 0); }
static void k_FindNextFile_(CPU* c, int wide) {
    FindH* f = find_get(ARG(0)); (void)wide;
    if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    if (++f->i >= f->n) { f->i = f->n; SETERR(E_NO_MORE_FILES); RET(0); return; }
    fill_find(f, ARG(1)); RET(1);
}
static void k_FindNextFileW(CPU* c) { k_FindNextFile_(c, 1); }
static void k_FindNextFileA(CPU* c) { k_FindNextFile_(c, 0); }
static void k_FindClose(CPU* c) {
    FindH* f = find_get(ARG(0));
    if (!f) { SETERR(E_INVALID_HANDLE); RET(0); return; }
    for (int i = 0; i < f->n; i++) { free(f->names[i]); if (f->ohost) free(f->ohost[i]); }
    free(f->names); free(f->ohost); free(f->oro); f->used = 0; RET(1);
}

/* ---------------- current directory, full paths, drives ---------------- */
static void k_GetCurrentDirectoryW(CPU* c) { ensure_init(); RET(put_str(g_cwd, 1, ARG(1), ARG(0))); }
static void k_GetCurrentDirectoryA(CPU* c) { ensure_init(); RET(put_str(g_cwd, 0, ARG(1), ARG(0))); }
static void k_SetCurrentDirectory_(CPU* c, int wide) {
    char p[1100], w[1100], host[1024]; int pok = 1; struct stat st;
    if (!garg(ARG(0), wide, p, sizeof p) || !vfs_map(p, w, host, sizeof host, &pok) || stat(host, &st)) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0); return; }
    if (!S_ISDIR(st.st_mode)) { SETERR(267); RET(0); return; }
    snprintf(g_cwd, sizeof g_cwd, "%s", w);
    if (g_trace) fprintf(stderr, "[vfs] SetCurrentDirectory(%s)\n", w);
    RET(1);
}
static void k_SetCurrentDirectoryW(CPU* c) { k_SetCurrentDirectory_(c, 1); }
static void k_SetCurrentDirectoryA(CPU* c) { k_SetCurrentDirectory_(c, 0); }
static void k_GetFullPathName_(CPU* c, int wide) {   /* (name, cap, buf, &filepart) */
    char p[1100], w[1100];
    if (!garg(ARG(0), wide, p, sizeof p)) { SETERR(E_INVALID_PARAMETER); RET(0); return; }
    ensure_init(); win_norm(p, w, sizeof w);
    size_t pl = strlen(p); int dir_like = pl && (p[pl - 1] == '\\' || p[pl - 1] == '/');
    if (dir_like && strlen(w) > 3) strcat(w, "\\");
    uint32_t r = put_str(w, wide, ARG(2), ARG(1));
    if (ARG(3) && r < ARG(1)) {
        const char* sl = strrchr(w, '\\');
        if (dir_like || !sl[1]) wr32(ARG(3), 0);
        else { char head[1100]; snprintf(head, sizeof head, "%.*s", (int)(sl + 1 - w), w); wr32(ARG(3), ARG(2) + units(head, wide) * (wide ? 2 : 1)); }
    }
    RET(r);
}
static void k_GetFullPathNameW(CPU* c) { k_GetFullPathName_(c, 1); }
static void k_GetFullPathNameA(CPU* c) { k_GetFullPathName_(c, 0); }
static void k_GetLongPathName_(CPU* c, int wide) {   /* (short, long, cap) */
    char p[1100], w[1100], host[1024]; int pok = 1;
    if (!garg(ARG(0), wide, p, sizeof p)) { SETERR(E_INVALID_PARAMETER); RET(0); return; }
    if (!vfs_map(p, w, host, sizeof host, &pok)) { SETERR(pok ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND); RET(0); return; }
    RET(put_str(w, wide, ARG(1), ARG(2)));
}
static void k_GetLongPathNameW(CPU* c) { k_GetLongPathName_(c, 1); }
static void k_GetLongPathNameA(CPU* c) { k_GetLongPathName_(c, 0); }
static int is_c_root(uint32_t p, int wide) { if (!p) return 1; unsigned ch = wide ? rd16(p) : rd8(p); return ch == 'C' || ch == 'c' || ch == '\\' || ch == '/'; }
static void k_GetDriveTypeW(CPU* c) { RET(is_c_root(ARG(0), 1) ? 3 /* DRIVE_FIXED */ : 1 /* DRIVE_NO_ROOT_DIR */); }
static void k_GetDriveTypeA(CPU* c) { RET(is_c_root(ARG(0), 0) ? 3 : 1); }
static void k_GetLogicalDrives(CPU* c) { RET(1u << 2); }
static void free_space(uint64_t* avail, uint64_t* total) {
    ensure_init(); struct statvfs v;
    if (statvfs(g_game, &v) == 0) { *avail = (uint64_t)v.f_bavail * v.f_frsize; *total = (uint64_t)v.f_blocks * v.f_frsize; }
    else { *avail = 8ull << 30; *total = 32ull << 30; }
}
static void k_GetDiskFreeSpace_(CPU* c) {   /* (root, sectors/cluster, bytes/sector, free clusters, total clusters) */
    uint64_t a, t; free_space(&a, &t);
    uint64_t fc = a / 4096, tc = t / 4096; if (fc > 0xFFFFFFFFu) fc = 0xFFFFFFFFu; if (tc > 0xFFFFFFFFu) tc = 0xFFFFFFFFu;
    if (ARG(1)) wr32(ARG(1), 8);
    if (ARG(2)) wr32(ARG(2), 512);
    if (ARG(3)) wr32(ARG(3), (uint32_t)fc);
    if (ARG(4)) wr32(ARG(4), (uint32_t)tc);
    RET(1);
}
static void k_GetDiskFreeSpaceEx_(CPU* c) {   /* (dir, avail, total, free) */
    uint64_t a, t; free_space(&a, &t);
    if (ARG(1)) wr64(ARG(1), a);
    if (ARG(2)) wr64(ARG(2), t);
    if (ARG(3)) wr64(ARG(3), a);
    RET(1);
}
#define TEMP_WIN USER_WIN "\\AppData\\Local\\Temp"
static void make_win_dir(const char* w) { char host[1024]; if (!vfs_map(w, NULL, host, sizeof host, NULL)) mkdirs(host); }
static void k_GetTempPathW(CPU* c) { make_win_dir(TEMP_WIN); RET(put_str(TEMP_WIN "\\", 1, ARG(1), ARG(0))); }
static void k_GetTempPathA(CPU* c) { make_win_dir(TEMP_WIN); RET(put_str(TEMP_WIN "\\", 0, ARG(1), ARG(0))); }

/* ---------------- time conversions (local time = UTC; pure arithmetic, no tzdata needed) ---------------- */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {   /* H. Hinnant's algorithm */
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, struct tm* t) {
    z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    t->tm_year = (int)(yoe + era * 400 + (m <= 2)) - 1900; t->tm_mon = m - 1; t->tm_mday = d;
    t->tm_wday = (int)(((z - 719468) % 7 + 11) % 7);   /* 1970-01-01 was a Thursday */
}
static void utc_tm(int64_t s, struct tm* t) {
    int64_t days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400), r = s - days * 86400;
    memset(t, 0, sizeof *t); civil_from_days(days, t);
    t->tm_hour = (int)(r / 3600); t->tm_min = (int)(r / 60 % 60); t->tm_sec = (int)(r % 60);
}
static void put_systime(uint32_t p, const struct tm* t, int ms) {
    wr16(p, t->tm_year + 1900); wr16(p + 2, t->tm_mon + 1); wr16(p + 4, t->tm_wday); wr16(p + 6, t->tm_mday);
    wr16(p + 8, t->tm_hour); wr16(p + 10, t->tm_min); wr16(p + 12, t->tm_sec); wr16(p + 14, ms);
}
static void k_FileTimeToSystemTime(CPU* c) {
    uint64_t ft = rd64(ARG(0));
    if (ft >= 0x8000000000000000ull) { SETERR(E_INVALID_PARAMETER); RET(0); return; }
    int64_t s = (int64_t)(ft / 10000000ull) - 11644473600ll; struct tm t; utc_tm(s, &t);
    put_systime(ARG(1), &t, (int)(ft % 10000000ull / 10000)); RET(1);
}
static void k_SystemTimeToFileTime(CPU* c) {
    uint32_t p = ARG(0); struct tm t; memset(&t, 0, sizeof t);
    t.tm_year = rd16(p) - 1900; t.tm_mon = rd16(p + 2) - 1; t.tm_mday = rd16(p + 6);
    t.tm_hour = rd16(p + 8); t.tm_min = rd16(p + 10); t.tm_sec = rd16(p + 12);
    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31) { SETERR(E_INVALID_PARAMETER); RET(0); return; }
    int64_t secs = days_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    wr64(ARG(1), ft_of((time_t)secs, 0) + (uint64_t)rd16(p + 14) * 10000ull); RET(1);
}
static void k_CopyFileTime(CPU* c) { wr64(ARG(1), rd64(ARG(0))); RET(1); }
static void k_GetSystemTime(CPU* c) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm t; utc_tm(ts.tv_sec, &t);
    put_systime(ARG(0), &t, (int)(ts.tv_nsec / 1000000));
}

/* ---------------- profile (ini) files ---------------- */
static int profile_path(uint32_t gp, int wide, char* w, char* host, size_t cap) {
    char p[1100], q[1100];
    if (!garg(gp, wide, p, sizeof p)) snprintf(p, sizeof p, "win.ini");
    if (!strchr(p, '\\') && !strchr(p, '/')) { snprintf(q, sizeof q, "C:\\WINDOWS\\%.1000s", p); memcpy(p, q, sizeof q); }
    return vfs_map(p, w, host, cap, NULL);
}
static void load_profile(const char* w, const char* host, int exists, Ini* ini) {
    uint8_t* b; size_t n;
    if (exists && patched_default(w, host, &b, &n)) { ini_load_mem(ini, b, n); free(b); return; }
    if (!exists || ini_load_file(ini, host)) ini_init(ini);
}
typedef struct { char* buf; size_t k, cap; } StrList;
static void list_add(const char* s, void* u) {
    StrList* l = u; size_t n = strlen(s) + 1;
    if (l->k + n + 1 > l->cap) { l->cap = (l->k + n + 1) * 2; l->buf = realloc(l->buf, l->cap); }
    memcpy(l->buf + l->k, s, n); l->k += n;
}
static void list_add_key(const char* k, const char* v, void* u) { (void)v; list_add(k, u); }
static uint32_t put_multi(const StrList* l, int wide, uint32_t dst, uint32_t cap) {   /* "a\0b\0\0" truncated like Win32 */
    uint32_t k = 0; if (cap < 2) { if (cap) { if (wide) wr16(dst, 0); else wr8(dst, 0); } return 0; }
    for (size_t i = 0; i < l->k;) {
        const char* s = l->buf + i; i += strlen(s) + 1;
        uint32_t n = units(s, wide);
        if (k + n + 2 > cap) { store(s, wide, dst + k * (wide ? 2 : 1), cap - 1 - k); k = cap - 2; if (wide) { wr16(dst + 2 * (cap - 1), 0); } else wr8(dst + cap - 1, 0); return cap - 2; }
        store(s, wide, dst + k * (wide ? 2 : 1), n + 1); k += n + 1;
    }
    if (wide) wr16(dst + 2 * k, 0); else wr8(dst + k, 0);
    return k;
}
static void k_GetPrivateProfileString_(CPU* c, int wide) {   /* (app, key, default, ret, size, file) */
    char app[300], key[300], def[1100], w[1100], host[1024], val[2048];
    uint32_t ret = ARG(3), size = ARG(4);
    if (!ret || !size) { RET(0); return; }
    int ex = profile_path(ARG(5), wide, w, host, sizeof host);
    Ini ini; load_profile(w, host, ex, &ini);
    uint32_t r;
    if (!ARG(0) || !ARG(1)) {
        StrList l = { 0 };
        if (!ARG(0)) ini_sections(&ini, list_add, &l);
        else { garg(ARG(0), wide, app, sizeof app); ini_keys(&ini, app, list_add_key, &l); }
        r = put_multi(&l, wide, ret, size); free(l.buf);
    } else {
        garg(ARG(0), wide, app, sizeof app); garg(ARG(1), wide, key, sizeof key);
        if (ini_get(&ini, app, key, val, sizeof val)) {
            size_t l = strlen(val);
            if (l >= 2 && (val[0] == '"' || val[0] == '\'') && val[l - 1] == val[0]) { memmove(val, val + 1, l - 2); val[l - 2] = 0; }
            SETERR(0);
        } else {
            if (!garg(ARG(2), wide, def, sizeof def)) def[0] = 0;
            size_t l = strlen(def); while (l && def[l - 1] == ' ') def[--l] = 0;
            snprintf(val, sizeof val, "%s", def); SETERR(E_FILE_NOT_FOUND);
        }
        store(val, wide, ret, size); r = units(val, wide); if (r > size - 1) r = size - 1;
    }
    ini_free(&ini);
    RET(r);
}
static void k_GetPrivateProfileStringW(CPU* c) { k_GetPrivateProfileString_(c, 1); }
static void k_GetPrivateProfileStringA(CPU* c) { k_GetPrivateProfileString_(c, 0); }
static void k_GetPrivateProfileInt_(CPU* c, int wide) {   /* (app, key, default, file) */
    char app[300], key[300], w[1100], host[1024], val[256];
    int ex = profile_path(ARG(3), wide, w, host, sizeof host);
    Ini ini; load_profile(w, host, ex, &ini);
    int32_t r = (int32_t)ARG(2);
    if (garg(ARG(0), wide, app, sizeof app) && garg(ARG(1), wide, key, sizeof key) && ini_get(&ini, app, key, val, sizeof val)) r = (int32_t)strtol(val, NULL, 10);
    ini_free(&ini); RET((uint32_t)r);
}
static void k_GetPrivateProfileIntW(CPU* c) { k_GetPrivateProfileInt_(c, 1); }
static void k_GetPrivateProfileIntA(CPU* c) { k_GetPrivateProfileInt_(c, 0); }
static void k_WritePrivateProfileString_(CPU* c, int wide) {   /* (app, key, string, file) */
    char app[300], key[300], val[2048], w[1100], host[1024];
    if (!ARG(0)) { RET(1); return; }   /* flush request */
    int ex = profile_path(ARG(3), wide, w, host, sizeof host);
    if (ex && t_ro) { SETERR(E_ACCESS_DENIED); RET(0); return; }   /* read-only mod ini */
    Ini ini; if (!ex || ini_load_file(&ini, host)) ini_init(&ini);
    garg(ARG(0), wide, app, sizeof app);
    if (!ARG(1)) ini_remove_section(&ini, app);
    else {
        garg(ARG(1), wide, key, sizeof key);
        if (!ARG(2)) ini_remove_key(&ini, app, key);
        else { garg(ARG(2), wide, val, sizeof val); ini_set(&ini, app, key, val); }
    }
    ensure_parent_root(w);
    int rc = ini_save_file(&ini, host); ini_free(&ini);
    if (!ex) dc_drop_parent(host);
    if (rc) { SETERR(E_ACCESS_DENIED); RET(0); return; }
    RET(1);
}
static void k_WritePrivateProfileStringW(CPU* c) { k_WritePrivateProfileString_(c, 1); }
static void k_WritePrivateProfileStringA(CPU* c) { k_WritePrivateProfileString_(c, 0); }

/* ---------------- environment ---------------- */
#define MAXENV 64
static char g_envk[MAXENV][64], g_envv[MAXENV][600]; static int g_nenv;
static void env_defaults(void) {
    if (g_nenv) return;
    static const char* const D[][2] = {
        { "ALLUSERSPROFILE", "C:\\ProgramData" }, { "APPDATA", USER_WIN "\\AppData\\Roaming" }, { "COMPUTERNAME", "ANDROID" },
        { "HOMEDRIVE", "C:" }, { "HOMEPATH", "\\Users\\Shepard" }, { "LOCALAPPDATA", USER_WIN "\\AppData\\Local" },
        { "NUMBER_OF_PROCESSORS", "8" }, { "OS", "Windows_NT" }, { "PROCESSOR_ARCHITECTURE", "x86" },
        { "ProgramData", "C:\\ProgramData" }, { "ProgramFiles", "C:\\Program Files" }, { "PUBLIC", "C:\\Users\\Public" },
        { "SystemDrive", "C:" }, { "SystemRoot", "C:\\WINDOWS" }, { "TEMP", TEMP_WIN }, { "TMP", TEMP_WIN },
        { "USERNAME", "Shepard" }, { "USERPROFILE", USER_WIN }, { "windir", "C:\\WINDOWS" },
        { "PATH", "C:\\MassEffect\\Binaries;C:\\WINDOWS\\system32;C:\\WINDOWS" } };
    for (unsigned i = 0; i < sizeof D / sizeof D[0]; i++) { snprintf(g_envk[i], 64, "%s", D[i][0]); snprintf(g_envv[i], 600, "%s", D[i][1]); }
    g_nenv = sizeof D / sizeof D[0];
}
static const char* env_get(const char* k) { env_defaults(); for (int i = 0; i < g_nenv; i++) if (!strcasecmp(g_envk[i], k)) return g_envv[i]; return NULL; }
static void env_set(const char* k, const char* v) {
    env_defaults();
    for (int i = 0; i < g_nenv; i++) if (!strcasecmp(g_envk[i], k)) {
        if (v) snprintf(g_envv[i], 600, "%s", v);
        else { g_nenv--; memmove(g_envk[i], g_envk[g_nenv], 64); memmove(g_envv[i], g_envv[g_nenv], 600); }
        return;
    }
    if (v && g_nenv < MAXENV) { snprintf(g_envk[g_nenv], 64, "%s", k); snprintf(g_envv[g_nenv], 600, "%s", v); g_nenv++; }
}
static void k_GetEnvironmentVariable_(CPU* c, int wide) {   /* (name, buf, size) */
    char k[128]; const char* v = garg(ARG(0), wide, k, sizeof k) ? env_get(k) : NULL;
    if (!v) { SETERR(E_ENVVAR_NOT_FOUND); RET(0); return; }
    RET(put_str(v, wide, ARG(1), ARG(2)));
}
static void k_GetEnvironmentVariableW(CPU* c) { k_GetEnvironmentVariable_(c, 1); }
static void k_GetEnvironmentVariableA(CPU* c) { k_GetEnvironmentVariable_(c, 0); }
static void k_SetEnvironmentVariable_(CPU* c, int wide) {
    char k[64], v[600]; if (!garg(ARG(0), wide, k, sizeof k)) { SETERR(E_INVALID_PARAMETER); RET(0); return; }
    env_set(k, garg(ARG(1), wide, v, sizeof v)); RET(1);
}
static void k_SetEnvironmentVariableW(CPU* c) { k_SetEnvironmentVariable_(c, 1); }
static void k_SetEnvironmentVariableA(CPU* c) { k_SetEnvironmentVariable_(c, 0); }
static void k_ExpandEnvironmentStrings_(CPU* c, int wide) {   /* (src, dst, size) -> chars incl. NUL */
    char s[2048], o[4096]; size_t k = 0;
    if (!garg(ARG(0), wide, s, sizeof s)) { SETERR(E_INVALID_PARAMETER); RET(0); return; }
    for (const char* p = s; *p && k + 1 < sizeof o;) {
        const char* e = *p == '%' ? strchr(p + 1, '%') : NULL;
        if (e && e > p + 1) {
            char name[128]; snprintf(name, sizeof name, "%.*s", (int)(e - p - 1), p + 1);
            const char* v = env_get(name);
            if (v) { k += snprintf(o + k, sizeof o - k, "%s", v); p = e + 1; continue; }
        }
        o[k++] = *p++;
    }
    o[k] = 0;
    uint32_t n = units(o, wide) + 1;
    if (ARG(1) && ARG(2) >= n) store(o, wide, ARG(1), ARG(2));
    RET(n);
}
static void k_ExpandEnvironmentStringsW(CPU* c) { k_ExpandEnvironmentStrings_(c, 1); }
static void k_ExpandEnvironmentStringsA(CPU* c) { k_ExpandEnvironmentStrings_(c, 0); }

/* ---------------- shell32 ---------------- */
static const char* csidl_path(uint32_t id) {
    switch (id & 0xFF) {
    case 0x00: case 0x10: return USER_WIN "\\Desktop";
    case 0x05: return DOCS_WIN;                                   /* CSIDL_PERSONAL (My Documents) */
    case 0x0D: return USER_WIN "\\Music";
    case 0x0E: return USER_WIN "\\Videos";
    case 0x27: return USER_WIN "\\Pictures";
    case 0x1A: return USER_WIN "\\AppData\\Roaming";             /* CSIDL_APPDATA */
    case 0x1C: return USER_WIN "\\AppData\\Local";               /* CSIDL_LOCAL_APPDATA */
    case 0x23: return "C:\\ProgramData";                          /* CSIDL_COMMON_APPDATA */
    case 0x24: return "C:\\WINDOWS";
    case 0x25: return "C:\\WINDOWS\\system32";
    case 0x26: return "C:\\Program Files";
    case 0x28: return USER_WIN;                                   /* CSIDL_PROFILE */
    case 0x2E: return "C:\\Users\\Public\\Documents";             /* CSIDL_COMMON_DOCUMENTS */
    default: return NULL;
    }
}
static uint32_t shell_folder(uint32_t id, int create, int wide, uint32_t dst) {
    ensure_init();
    const char* w = csidl_path(id);
    if (!w) { if (dst) store("", wide, dst, 260); return 0x80070057u; }   /* E_INVALIDARG */
    char host[1024]; int ex = vfs_map(w, NULL, host, sizeof host, NULL);
    if (!ex && (create || (id & 0xFF) == 0x05 || (id & 0xFF) == 0x1A || (id & 0xFF) == 0x1C)) { mkdirs(host); ex = 1; }
    if (dst) store(w, wide, dst, 260);
    if (g_trace) fprintf(stderr, "[vfs] SHGetFolderPath(CSIDL 0x%02x) -> %s  [%s]\n", id & 0xFF, w, host);
    return ex ? 0 : 0x80070002u;   /* S_OK / HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */
}
static void s_SHGetFolderPathW(CPU* c) { RET(shell_folder(ARG(1), ARG(1) & 0x8000u, 1, ARG(4))); }   /* (hwnd, csidl, token, flags, path) */
static void s_SHGetFolderPathA(CPU* c) { RET(shell_folder(ARG(1), ARG(1) & 0x8000u, 0, ARG(4))); }
static void s_SHGetSpecialFolderPathW(CPU* c) { RET(shell_folder(ARG(2), ARG(3), 1, ARG(1)) == 0); }   /* (hwnd, path, csidl, create) */
static void s_SHGetSpecialFolderPathA(CPU* c) { RET(shell_folder(ARG(2), ARG(3), 0, ARG(1)) == 0); }
static void s_ShellExecute_(CPU* c, int wide) {   /* (hwnd, verb, file, params, dir, show): log only (web links, readme) */
    char v[64], f[1100];
    fprintf(stderr, "[hle] ShellExecute(%s, \"%s\") ignored\n", garg(ARG(1), wide, v, sizeof v) ? v : "open", garg(ARG(2), wide, f, sizeof f) ? f : "");
    RET(42);   /* > 32 = success */
}
static void s_ShellExecuteW(CPU* c) { s_ShellExecute_(c, 1); }
static void s_ShellExecuteA(CPU* c) { s_ShellExecute_(c, 0); }

const HleDef hle_fs_defs[] = {
#define K(n, a, f) { "kernel32.dll", n, a, f },
    K("CreateFileW", 7, k_CreateFileW) K("CreateFileA", 7, k_CreateFileA) K("ReadFile", 5, k_ReadFile) K("WriteFile", 5, k_WriteFile)
    K("SetFilePointer", 4, k_SetFilePointer) K("SetFilePointerEx", 5, k_SetFilePointerEx)
    K("GetFileSize", 2, k_GetFileSize) K("GetFileSizeEx", 2, k_GetFileSizeEx) K("SetEndOfFile", 1, k_SetEndOfFile)
    K("FlushFileBuffers", 1, k_FlushFileBuffers) K("GetFileTime", 4, k_GetFileTime) K("SetFileTime", 4, k_SetFileTime)
    K("GetFileInformationByHandle", 2, k_GetFileInformationByHandle) K("CloseHandle", 1, k_CloseHandle)
    K("LockFile", 5, k_Ret1) K("UnlockFile", 5, k_Ret1)
    K("GetFileAttributesW", 1, k_GetFileAttributesW) K("GetFileAttributesA", 1, k_GetFileAttributesA)
    K("GetFileAttributesExW", 3, k_GetFileAttributesExW) K("GetFileAttributesExA", 3, k_GetFileAttributesExA)
    K("SetFileAttributesW", 2, k_SetFileAttributesW) K("SetFileAttributesA", 2, k_SetFileAttributesA)
    K("CreateDirectoryW", 2, k_CreateDirectoryW) K("CreateDirectoryA", 2, k_CreateDirectoryA)
    K("RemoveDirectoryW", 1, k_RemoveDirectoryW) K("RemoveDirectoryA", 1, k_RemoveDirectoryA)
    K("DeleteFileW", 1, k_DeleteFileW) K("DeleteFileA", 1, k_DeleteFileA)
    K("MoveFileW", 2, k_MoveFileW) K("MoveFileA", 2, k_MoveFileA) K("MoveFileExW", 3, k_MoveFileExW) K("MoveFileExA", 3, k_MoveFileExA)
    K("CopyFileW", 3, k_CopyFileW) K("CopyFileA", 3, k_CopyFileA)
    K("FindFirstFileW", 2, k_FindFirstFileW) K("FindFirstFileA", 2, k_FindFirstFileA)
    K("FindNextFileW", 2, k_FindNextFileW) K("FindNextFileA", 2, k_FindNextFileA) K("FindClose", 1, k_FindClose)
    K("GetCurrentDirectoryW", 2, k_GetCurrentDirectoryW) K("GetCurrentDirectoryA", 2, k_GetCurrentDirectoryA)
    K("SetCurrentDirectoryW", 1, k_SetCurrentDirectoryW) K("SetCurrentDirectoryA", 1, k_SetCurrentDirectoryA)
    K("GetFullPathNameW", 4, k_GetFullPathNameW) K("GetFullPathNameA", 4, k_GetFullPathNameA)
    K("GetLongPathNameW", 3, k_GetLongPathNameW) K("GetLongPathNameA", 3, k_GetLongPathNameA)
    K("GetDriveTypeW", 1, k_GetDriveTypeW) K("GetDriveTypeA", 1, k_GetDriveTypeA) K("GetLogicalDrives", 0, k_GetLogicalDrives)
    K("GetDiskFreeSpaceA", 5, k_GetDiskFreeSpace_) K("GetDiskFreeSpaceW", 5, k_GetDiskFreeSpace_)
    K("GetDiskFreeSpaceExA", 4, k_GetDiskFreeSpaceEx_) K("GetDiskFreeSpaceExW", 4, k_GetDiskFreeSpaceEx_)
    K("GetTempPathW", 2, k_GetTempPathW) K("GetTempPathA", 2, k_GetTempPathA)
    K("FileTimeToSystemTime", 2, k_FileTimeToSystemTime) K("SystemTimeToFileTime", 2, k_SystemTimeToFileTime)
    K("FileTimeToLocalFileTime", 2, k_CopyFileTime) K("LocalFileTimeToFileTime", 2, k_CopyFileTime)
    K("GetSystemTime", 1, k_GetSystemTime) K("GetLocalTime", 1, k_GetSystemTime)
    K("GetPrivateProfileStringW", 6, k_GetPrivateProfileStringW) K("GetPrivateProfileStringA", 6, k_GetPrivateProfileStringA)
    K("GetPrivateProfileIntW", 4, k_GetPrivateProfileIntW) K("GetPrivateProfileIntA", 4, k_GetPrivateProfileIntA)
    K("WritePrivateProfileStringW", 4, k_WritePrivateProfileStringW) K("WritePrivateProfileStringA", 4, k_WritePrivateProfileStringA)
    K("GetEnvironmentVariableW", 3, k_GetEnvironmentVariableW) K("GetEnvironmentVariableA", 3, k_GetEnvironmentVariableA)
    K("SetEnvironmentVariableW", 2, k_SetEnvironmentVariableW) K("SetEnvironmentVariableA", 2, k_SetEnvironmentVariableA)
    K("ExpandEnvironmentStringsW", 3, k_ExpandEnvironmentStringsW) K("ExpandEnvironmentStringsA", 3, k_ExpandEnvironmentStringsA)
#undef K
#define S(n, a, f) { "shell32.dll", n, a, f },
    S("SHGetFolderPathW", 5, s_SHGetFolderPathW) S("SHGetFolderPathA", 5, s_SHGetFolderPathA)
    S("SHGetSpecialFolderPathW", 4, s_SHGetSpecialFolderPathW) S("SHGetSpecialFolderPathA", 4, s_SHGetSpecialFolderPathA)
    S("ShellExecuteW", 6, s_ShellExecuteW) S("ShellExecuteA", 6, s_ShellExecuteA)
#undef S
};
const unsigned hle_fs_ndefs = sizeof hle_fs_defs / sizeof hle_fs_defs[0];
