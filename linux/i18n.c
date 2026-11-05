/* linux/i18n.c - see i18n.h */
#define _GNU_SOURCE
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

const char* const I18N_CODES[I18N_N] = { "ru", "en", "de", "fr", "it", "es", "pl", "cs", "hu" };
const char* const I18N_NAMES[I18N_N] = { "Русский", "English", "Deutsch", "Français", "Italiano", "Español", "Polski", "Čeština", "Magyar" };
const char* const I18N_GAME[I18N_N] = { "RU", "INT", "DE", "FR", "IT", "ES", "PL", "CZ", "HU" };

extern const char me1a_tr_en[], me1a_tr_de[], me1a_tr_fr[], me1a_tr_it[], me1a_tr_es[], me1a_tr_pl[], me1a_tr_cs[], me1a_tr_hu[];
extern const char me1a_lx_en[], me1a_lx_de[], me1a_lx_fr[], me1a_lx_it[], me1a_lx_es[], me1a_lx_pl[], me1a_lx_cs[], me1a_lx_hu[];
static const char* const AND[I18N_N] = { 0, me1a_tr_en, me1a_tr_de, me1a_tr_fr, me1a_tr_it, me1a_tr_es, me1a_tr_pl, me1a_tr_cs, me1a_tr_hu };
static const char* const LNX[I18N_N] = { 0, me1a_lx_en, me1a_lx_de, me1a_lx_fr, me1a_lx_it, me1a_lx_es, me1a_lx_pl, me1a_lx_cs, me1a_lx_hu };

typedef struct { char** k; char** v; unsigned cap, n; } Map;
static Map g_tr, g_en; static int g_cur;
static uint32_t hs(const char* s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static void map_put(Map* m, char* k, char* v) {
    if ((m->n + 1) * 2 > m->cap) {
        Map o = *m; m->cap = m->cap ? m->cap * 2 : 1024; m->n = 0;
        m->k = calloc(m->cap, sizeof(char*)); m->v = calloc(m->cap, sizeof(char*));
        for (unsigned i = 0; i < o.cap; i++) if (o.k[i]) map_put(m, o.k[i], o.v[i]);
        free(o.k); free(o.v);
    }
    for (uint32_t h = hs(k);; h++) {
        unsigned i = h & (m->cap - 1);
        if (!m->k[i]) { m->k[i] = k; m->v[i] = v; m->n++; return; }
        if (!strcmp(m->k[i], k)) { free(m->v[i]); free(k); m->v[i] = v; return; }   /* later file wins (linux over android) */
    }
}
static const char* map_get(const Map* m, const char* k) {
    if (!m->cap) return NULL;
    for (uint32_t h = hs(k);; h++) { unsigned i = h & (m->cap - 1); if (!m->k[i]) return NULL; if (!strcmp(m->k[i], k)) return m->v[i]; }
}
static void map_free(Map* m) { for (unsigned i = 0; i < m->cap; i++) if (m->k[i]) { free(m->k[i]); free(m->v[i]); } free(m->k); free(m->v); memset(m, 0, sizeof *m); }
/* a JSON string at *p (after the quote) -> malloc'd UTF-8 */
static char* jstr(const char** pp) {
    const char* p = *pp; size_t cap = 64, n = 0; char* o = malloc(cap);
    while (*p && *p != '"') {
        if (n + 8 > cap) { cap *= 2; o = realloc(o, cap); }
        if (*p != '\\') { o[n++] = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': o[n++] = '\n'; p++; break; case 't': o[n++] = '\t'; p++; break; case 'r': o[n++] = '\r'; p++; break;
        case 'b': o[n++] = '\b'; p++; break; case 'f': o[n++] = '\f'; p++; break;
        case 'u': {
            unsigned c = 0; p++;
            for (int i = 0; i < 4 && *p; i++, p++) c = c * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
            if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                unsigned d = 0; p += 2; for (int i = 0; i < 4 && *p; i++, p++) d = d * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
            }
            if (c < 0x80) o[n++] = (char)c;
            else if (c < 0x800) { o[n++] = (char)(0xC0 | c >> 6); o[n++] = (char)(0x80 | (c & 63)); }
            else if (c < 0x10000) { o[n++] = (char)(0xE0 | c >> 12); o[n++] = (char)(0x80 | (c >> 6 & 63)); o[n++] = (char)(0x80 | (c & 63)); }
            else { o[n++] = (char)(0xF0 | c >> 18); o[n++] = (char)(0x80 | (c >> 12 & 63)); o[n++] = (char)(0x80 | (c >> 6 & 63)); o[n++] = (char)(0x80 | (c & 63)); }
            break; }
        default: if (*p) o[n++] = *p++; break;
        }
    }
    if (*p == '"') p++;
    o[n] = 0; *pp = p; return o;
}
static void load(Map* m, const char* js) {
    const char* p = js;
    if (!p) return;
    while (*p) {
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++; char* k = jstr(&p);
        while (*p && *p != ':') p++;
        if (!*p) { free(k); break; }
        p++; while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != '"') { free(k); continue; }
        p++; char* v = jstr(&p);
        map_put(m, k, v);
    }
}
void i18n_set(int lang) {
    if (lang < 0 || lang >= I18N_N) lang = 0;
    map_free(&g_tr); map_free(&g_en); g_cur = lang;
    if (lang == 0) return;
    load(&g_en, AND[1]); load(&g_en, LNX[1]);
    if (lang > 1) { load(&g_tr, AND[lang]); load(&g_tr, LNX[lang]); }
}
int i18n_cur(void) { return g_cur; }
int i18n_find(const char* code) {
    if (!code) return -1;
    if (!strncmp(code, "cz", 2)) return 7;
    for (int i = 0; i < I18N_N; i++) if (!strncmp(code, I18N_CODES[i], 2)) return i;
    return -1;
}
int i18n_system(void) {
    const char* e[] = { getenv("LC_ALL"), getenv("LC_MESSAGES"), getenv("LANG") };
    for (int i = 0; i < 3; i++) if (e[i] && *e[i] && strcmp(e[i], "C") && strcmp(e[i], "POSIX")) { int k = i18n_find(e[i]); return k >= 0 ? k : 1; }
    return 1;
}
const char* T(const char* s) {
    if (!s || g_cur == 0) return s;
    const char* r = map_get(&g_tr, s);
    if (!r) r = map_get(&g_en, s);
    return r ? r : s;
}
/* fragments of native messages (settings/me1settings.c and the launcher's reports): as FRAGS in android L.java */
static const char* const FRAGS[] = {
    "GPU не распознан (", " → класс «", "; мало ОЗУ → «", "; CPU до ", " ГГц → «", "ОЗУ ", " ГБ, ", " ядер", "до ", " ГГц, ",
    "экран ", "рендер ", "минимальный", "низкий", "средний", "высокий", "максимальный",
    "ошибка: ", "не удалось создать папку для ", "ошибка записи ", "переопределения: ",
    ".ini ещё не создан игрой — значения войдут в него при первом запуске", "обновлён: ", "настройки не загружены",
    "Папка игры: ", "Мои документы (для игры): ", " (ошибка)", "BIOEngine.ini (создан игрой)", "DefaultEngine.ini + настройки порта",
    "Конфигурация игры не найдена — скопируйте игру в папку выше.", "Игра прочитает (",
    "Разрешение X", "Разрешение Y", "Детализация текстур", "Фильтрация", "Динамические тени", "Постобработка", "Каналов звука", "Гамма", "Язык",
    ": ожидается 0/1", ": ожидается целое", ": допустимо ", ": ожидается число", ": формат ШИРИНАxВЫСОТА, от 320x200",
    ": недопустимое значение '", "неизвестная настройка '", "неизвестный пресет '",
    "встроенная", "дискретная", "программная (CPU)", "виртуальная", " потоков", "видеопамять ", "Vulkan нет", NULL };
static int u8len(const char* s) { int n = 0; for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80; return n; }
char* i18n_tx(const char* s) {
    if (!s) return NULL;
    char* cur = strdup(s);
    if (g_cur == 0) return cur;
    int n = 0; while (FRAGS[n]) n++;
    int* ord = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strlen(FRAGS[ord[j]]) > strlen(FRAGS[ord[i]])) { int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    for (int k = 0; k < n; k++) {
        const char* f = FRAGS[ord[k]]; const char* r = T(f);
        if (!strstr(cur, f) || r == f) continue;
        size_t fl = strlen(f), rl = strlen(r), cap = strlen(cur) * 2 + 64; char* o = malloc(cap); size_t on = 0;
        const char* p = cur, *q;
        while ((q = strstr(p, f))) {
            size_t pre = (size_t)(q - p);
            if (on + pre + rl + 64 > cap) { cap = (on + pre + rl + 64) * 2; o = realloc(o, cap); }
            memcpy(o + on, p, pre); on += pre; memcpy(o + on, r, rl); on += rl; p = q + fl;
            int sp = 0; while (p[sp] == ' ') sp++;
            if (sp >= 2) {   /* padded column: keep values aligned */
                int pad = u8len(f) + sp - u8len(r); if (pad < 1) pad = 1;
                if (on + (size_t)pad + 8 > cap) { cap = (on + pad + 8) * 2; o = realloc(o, cap); }
                memset(o + on, ' ', (size_t)pad); on += (size_t)pad; p += sp;
            }
        }
        size_t rest = strlen(p);
        if (on + rest + 1 > cap) { cap = on + rest + 1; o = realloc(o, cap); }
        memcpy(o + on, p, rest + 1);
        free(cur); cur = o;
    }
    free(ord);
    return cur;
}
