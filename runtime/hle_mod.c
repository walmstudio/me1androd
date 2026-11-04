/* PhoneModify: user-made engine modifiers for weak phones (v0.10.4).
 * A modifier is a small text file in <game>/PhoneModify/*.txt|*.ini. Directives (one per line, # starts a comment):
 *   maxdraws <n>            skip draw calls beyond n per frame (the tail of the frame is not drawn)
 *   maxprims <n>            skip draws once the frame has emitted n primitives (same idea, tuned by primitives)
 *   ini <section> <key> <value>   extra [SystemSettings]-style key substituted into BIOEngine.ini in memory
 *                                 (like "simplified calculation", but from the modifier and independently of it)
 *   patch <hexaddr> <hex bytes ...>   write bytes into guest memory once that address is mapped (MassEffect.exe
 *                                 is rebased to 0x10900000, addresses in the log are the ones to use here)
 * Files are applied only when the "Модификаторы PhoneModify" launcher switch is on (ME1_PHONE_MOD=1). The generator:
 * `me1cfg --game DIR modgen balanced|aggressive|off` writes PhoneModify/phone_tune.txt. */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#define MOD_PATCH_MAX 64
#define MOD_BYTES_MAX 32
typedef struct { uint32_t va, n; uint8_t bytes[MOD_BYTES_MAX]; int done, logged; } ModPatch;
static ModPatch g_patch[MOD_PATCH_MAX];
static unsigned g_npatch, g_maxdraws, g_maxprims;
static int g_on = -1;
static char g_dir[1024];

int hle_mod_on(void) {
    if (g_on < 0) { const char* e = getenv("ME1_PHONE_MOD"); g_on = e && *e == '1'; }
    return g_on;
}
unsigned hle_mod_maxdraws(void) { return g_maxdraws; }
unsigned hle_mod_maxprims(void) { return g_maxprims; }
void hle_fs_mod_keys_clear(void);
void hle_fs_mod_key(const char* sec, const char* key, const char* val, char how);

static void parse_file(const char* path, const char* name) {
    FILE* f = fopen(path, "r"); if (!f) return;
    char line[600]; int n = 0, errs = 0;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || !*p) continue;
        for (char* q = p + strlen(p); q > p && (q[-1] == '\n' || q[-1] == '\r' || q[-1] == ' ' || q[-1] == '\t'); q--) q[-1] = 0;
        char d[32]; int k = 0; while (p[k] && p[k] != ' ' && p[k] != '\t' && k < (int)sizeof d - 1) { d[k] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]); k++; }
        d[k] = 0;
        if (!strcmp(d, "maxdraws") || !strcmp(d, "maxprims")) {
            unsigned v = (unsigned)strtoul(p + k, NULL, 10);
            if (!strcmp(d, "maxdraws")) g_maxdraws = v; else g_maxprims = v;
        } else if (!strcmp(d, "ini")) {
            char sec[64] = "", key[64] = "", val[128] = "";
            if (sscanf(p + k, " %63s %63s %127[^\n]", sec, key, val) >= 2 && sec[0] && key[0]) {
                hle_fs_mod_key(sec, key, val[0] ? val : "", '=');
            } else errs++;
        } else if (!strcmp(d, "patch")) {
            char* q = p + k; char* end;
            uint32_t va = (uint32_t)strtoul(q, &end, 16);
            if (end == q || !va || g_npatch >= MOD_PATCH_MAX) { errs++; continue; }
            ModPatch* pt = &g_patch[g_npatch];
            unsigned nb = 0;
            for (q = end; *q && nb < MOD_BYTES_MAX; ) {
                while (*q == ' ' || *q == '\t' || *q == ',') q++;
                if (!*q) break;
                unsigned long b = strtoul(q, &end, 16);
                if (end == q || b > 255) { nb = 0; break; }
                pt->bytes[nb++] = (uint8_t)b; q = end;
            }
            if (nb) { pt->va = va; pt->n = nb; pt->done = pt->logged = 0; g_npatch++; } else errs++;
        } else { errs++; continue; }
        n++;
    }
    fclose(f);
    fprintf(stderr, "[mod] PhoneModify: %s: %d directive%s%s\n", name, n, n == 1 ? "" : "s", errs ? " (unknown lines ignored)" : "");
}
void hle_mod_init(const char* game_dir) {
    g_on = -1; g_maxdraws = g_maxprims = 0; g_npatch = 0; g_dir[0] = 0;
    hle_fs_mod_keys_clear();
    if (!hle_mod_on()) return;   /* the launcher switch; do not scan or log anything when off */
    if (!game_dir || !*game_dir) { const char* e = getenv("ME1_GAME"); game_dir = e && *e ? e : "."; }
    snprintf(g_dir, sizeof g_dir, "%s/PhoneModify", game_dir);
    DIR* dp = opendir(g_dir);
    if (!dp) { fprintf(stderr, "[mod] PhoneModify on, but %s does not exist (nothing to apply)\n", g_dir); return; }
    struct dirent* de;
    while ((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if (l < 5 || (strcasecmp(de->d_name + l - 4, ".txt") && strcasecmp(de->d_name + l - 4, ".ini"))) continue;
        char path[1200]; snprintf(path, sizeof path, "%s/%s", g_dir, de->d_name);
        parse_file(path, de->d_name);
    }
    closedir(dp);
    if (g_maxdraws || g_maxprims || g_npatch || hle_fs_mod_key_count()) {
        fprintf(stderr, "[mod] PhoneModify active: maxdraws %u, maxprims %u, %u patch(es), %u ini key(s)\n",
                g_maxdraws, g_maxprims, g_npatch, hle_fs_mod_key_count());
    } else fprintf(stderr, "[mod] PhoneModify on, no usable directives found in %s\n", g_dir);
}
/* write queued patches whose address is mapped by now; called from present()'s 2 s tick (and tests) */
void hle_mod_apply(void) {
    if (!hle_mod_on()) return;
    for (unsigned i = 0; i < g_npatch; i++) {
        ModPatch* p = &g_patch[i];
        if (p->done) continue;
        uint32_t base, size;
        if (!hle_vmem_query(p->va, &base, &size) || (uint64_t)p->va + p->n > (uint64_t)base + size) continue;
        for (uint32_t k = 0; k < p->n; k++) wr8(p->va + k, p->bytes[k]);
        p->done = 1;
        if (!p->logged) { p->logged = 1; fprintf(stderr, "[mod] patch %08x: %u bytes written\n", p->va, p->n); }
    }
}
const char* hle_mod_dir(void) { return g_dir; }
