#define _GNU_SOURCE
#include "ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

void ini_init(Ini* ini) { memset(ini, 0, sizeof *ini); ini->crlf = 1; }
void ini_free(Ini* ini) { for (int i = 0; i < ini->n; i++) free(ini->lines[i]); free(ini->lines); ini_init(ini); }
static void push_line(Ini* ini, const char* s, size_t len) {
    ini->noeol = 0;
    if (ini->n == ini->cap) { ini->cap = ini->cap ? ini->cap * 2 : 64; ini->lines = realloc(ini->lines, ini->cap * sizeof(char*)); }
    char* p = malloc(len + 1); memcpy(p, s, len); p[len] = 0; ini->lines[ini->n++] = p;
}
static void insert_line(Ini* ini, int at, const char* s) {
    int noeol = ini->noeol;
    push_line(ini, s, strlen(s));
    ini->noeol = noeol;
    char* last = ini->lines[ini->n - 1];
    memmove(ini->lines + at + 1, ini->lines + at, (ini->n - 1 - at) * sizeof(char*));
    ini->lines[at] = last;
}
static void delete_line(Ini* ini, int at) { free(ini->lines[at]); memmove(ini->lines + at, ini->lines + at + 1, (ini->n - at - 1) * sizeof(char*)); ini->n--; }

/* UTF-16LE -> UTF-8 */
static char* utf16_to_utf8(const unsigned char* p, size_t n, size_t* outn) {
    char* o = malloc(n * 2 + 4); size_t k = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned cp = p[i] | p[i + 1] << 8;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) { unsigned lo = p[i + 2] | p[i + 3] << 8; if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; } }
        if (cp < 0x80) o[k++] = cp;
        else if (cp < 0x800) { o[k++] = 0xC0 | cp >> 6; o[k++] = 0x80 | (cp & 63); }
        else if (cp < 0x10000) { o[k++] = 0xE0 | cp >> 12; o[k++] = 0x80 | (cp >> 6 & 63); o[k++] = 0x80 | (cp & 63); }
        else { o[k++] = 0xF0 | cp >> 18; o[k++] = 0x80 | (cp >> 12 & 63); o[k++] = 0x80 | (cp >> 6 & 63); o[k++] = 0x80 | (cp & 63); }
    }
    *outn = k; return o;
}
int ini_load_mem(Ini* ini, const void* data, size_t n) {
    ini_init(ini);
    const unsigned char* p = data; char* buf = NULL; size_t len = n;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { ini->utf16 = 1; buf = utf16_to_utf8(p + 2, n - 2, &len); }
    else { if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { ini->bom8 = 1; p += 3; len -= 3; } buf = malloc(len + 1); memcpy(buf, p, len); }
    int crlf = 0, lf = 0; size_t s = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            size_t e = i; if (e > s && buf[e - 1] == '\r') { e--; crlf++; } else if (i < len) lf++;
            if (i < len || e > s) push_line(ini, buf + s, e - s);
            if (i == len && e > s) ini->noeol = 1;
            s = i + 1;
        }
    }
    ini->crlf = crlf >= lf;
    free(buf); return 0;
}
int ini_load_file(Ini* ini, const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) { ini_init(ini); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = malloc(n > 0 ? n : 1); size_t got = fread(b, 1, n, f); fclose(f);
    ini_load_mem(ini, b, got); free(b); return 0;
}
char* ini_serialize(const Ini* ini, size_t* outn) {
    size_t total = 4; for (int i = 0; i < ini->n; i++) total += strlen(ini->lines[i]) + 2;
    char* u = malloc(total); size_t k = 0;
    if (ini->bom8) { u[k++] = 0xEF; u[k++] = 0xBB; u[k++] = 0xBF; }
    for (int i = 0; i < ini->n; i++) {
        size_t l = strlen(ini->lines[i]); memcpy(u + k, ini->lines[i], l); k += l;
        if (ini->noeol && i == ini->n - 1) break;
        if (ini->crlf) u[k++] = '\r';
        u[k++] = '\n';
    }
    if (!ini->utf16) { *outn = k; return u; }
    unsigned char* w = malloc(k * 4 + 4); size_t j = 0; w[j++] = 0xFF; w[j++] = 0xFE;
    for (size_t i = 0; i < k;) {
        unsigned char ch = u[i]; unsigned cp; int extra;
        if (ch < 0x80) { cp = ch; extra = 0; } else if ((ch & 0xE0) == 0xC0) { cp = ch & 31; extra = 1; } else if ((ch & 0xF0) == 0xE0) { cp = ch & 15; extra = 2; } else { cp = ch & 7; extra = 3; }
        i++; while (extra-- > 0 && i < k) cp = cp << 6 | (u[i++] & 63);
        if (cp >= 0x10000) { cp -= 0x10000; unsigned hi = 0xD800 + (cp >> 10), lo = 0xDC00 + (cp & 1023); w[j++] = hi; w[j++] = hi >> 8; w[j++] = lo; w[j++] = lo >> 8; }
        else { w[j++] = cp; w[j++] = cp >> 8; }
    }
    free(u); *outn = j; return (char*)w;
}
int ini_save_file(const Ini* ini, const char* path) {
    size_t n; char* b = ini_serialize(ini, &n);
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb"); if (!f) { free(b); return -1; }
    int ok = fwrite(b, 1, n, f) == n; ok &= fclose(f) == 0; free(b);
    if (!ok || rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}
/* ---- parsing helpers ---- */
static const char* skip_ws(const char* s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int section_name(const char* line, char* out, size_t cap) {   /* "[Name]" -> Name */
    const char* s = skip_ws(line); if (*s != '[') return 0;
    const char* e = strchr(s, ']'); if (!e) return 0;
    size_t l = e - s - 1; if (l >= cap) l = cap - 1; memcpy(out, s + 1, l); out[l] = 0; return 1;
}
/* key line "Key=Value" (UE3 also allows spaces around '='). Prefixed (+,-,.,!) and comment lines are not plain keys. */
static int key_value(const char* line, char* key, size_t kcap, const char** val) {
    const char* s = skip_ws(line);
    if (!*s || *s == ';' || *s == '#' || *s == '[' || *s == '+' || *s == '-' || *s == '.' || *s == '!') return 0;
    const char* eq = strchr(s, '='); if (!eq) return 0;
    const char* e = eq; while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    size_t l = e - s; if (l >= kcap) l = kcap - 1; memcpy(key, s, l); key[l] = 0;
    *val = skip_ws(eq + 1); return 1;
}
static int find_section(const Ini* ini, const char* sec, int from) {
    char name[256];
    for (int i = from; i < ini->n; i++) if (section_name(ini->lines[i], name, sizeof name) && !strcasecmp(name, sec)) return i;
    return -1;
}
static int section_end(const Ini* ini, int start) {   /* index of next section header or n */
    char name[256];
    for (int i = start + 1; i < ini->n; i++) if (section_name(ini->lines[i], name, sizeof name)) return i;
    return ini->n;
}
int ini_has_section(const Ini* ini, const char* sec) { return find_section(ini, sec, 0) >= 0; }
int ini_get(const Ini* ini, const char* sec, const char* key, char* out, size_t cap) {
    char k[256]; const char* v;
    for (int s = find_section(ini, sec, 0); s >= 0; s = find_section(ini, sec, s + 1)) {
        int e = section_end(ini, s);
        for (int i = s + 1; i < e; i++)
            if (key_value(ini->lines[i], k, sizeof k, &v) && !strcasecmp(k, key)) {
                size_t l = strlen(v); while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
                if (l >= cap) l = cap - 1;
                memcpy(out, v, l); out[l] = 0; return 1;
            }
    }
    return 0;
}
void ini_set(Ini* ini, const char* sec, const char* key, const char* val) {
    char k[256], line[2048]; const char* v; int found = 0;
    for (int s = find_section(ini, sec, 0); s >= 0; s = find_section(ini, sec, s + 1)) {
        int e = section_end(ini, s);
        for (int i = s + 1; i < e; i++)
            if (key_value(ini->lines[i], k, sizeof k, &v) && !strcasecmp(k, key)) {
                if (!found) { snprintf(line, sizeof line, "%s=%s", k, val); free(ini->lines[i]); ini->lines[i] = strdup(line); found = 1; }
                else { delete_line(ini, i); i--; e--; }
            }
    }
    if (found) return;
    snprintf(line, sizeof line, "%s=%s", key, val);
    int s = find_section(ini, sec, 0);
    if (s < 0) {
        if (ini->n && *skip_ws(ini->lines[ini->n - 1])) push_line(ini, "", 0);
        char h[300]; snprintf(h, sizeof h, "[%s]", sec); push_line(ini, h, strlen(h)); push_line(ini, line, strlen(line)); return;
    }
    int e = section_end(ini, s), at = e;
    while (at > s + 1 && !*skip_ws(ini->lines[at - 1])) at--;     /* before trailing blank lines */
    insert_line(ini, at, line);
}
int ini_remove_key(Ini* ini, const char* sec, const char* key) {
    char k[256]; const char* v; int removed = 0;
    for (int s = find_section(ini, sec, 0); s >= 0; s = find_section(ini, sec, s + 1)) {
        int e = section_end(ini, s);
        for (int i = s + 1; i < e; i++) if (key_value(ini->lines[i], k, sizeof k, &v) && !strcasecmp(k, key)) { delete_line(ini, i); i--; e--; removed++; }
    }
    return removed;
}
int ini_remove_section(Ini* ini, const char* sec) {
    int removed = 0, s;
    while ((s = find_section(ini, sec, 0)) >= 0) { int e = section_end(ini, s); for (int i = e - 1; i >= s; i--) delete_line(ini, i); removed++; }
    return removed;
}
void ini_sections(const Ini* ini, void (*cb)(const char*, void*), void* u) {
    char name[256];
    for (int i = 0; i < ini->n; i++) if (section_name(ini->lines[i], name, sizeof name)) cb(name, u);
}
void ini_keys(const Ini* ini, const char* sec, void (*cb)(const char*, const char*, void*), void* u) {
    char k[256]; const char* v;
    for (int s = find_section(ini, sec, 0); s >= 0; s = find_section(ini, sec, s + 1)) {
        int e = section_end(ini, s);
        for (int i = s + 1; i < e; i++) if (key_value(ini->lines[i], k, sizeof k, &v)) cb(k, v, u);
    }
}
int ini_parse_bool(const char* v, int def) {
    v = skip_ws(v);
    if (!strncasecmp(v, "true", 4) || !strncasecmp(v, "yes", 3) || !strncasecmp(v, "on", 2)) return 1;
    if (!strncasecmp(v, "false", 5) || !strncasecmp(v, "no", 2) || !strncasecmp(v, "off", 3)) return 0;
    if (isdigit((unsigned char)*v) || *v == '-') return atoi(v) != 0;
    return def;
}
