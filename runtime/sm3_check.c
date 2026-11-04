/* Offline check of the shader translator on shaders dumped by the game (ME1_D3D_DUMP=dir -> dir/shaders.pak, or single
 * .bin bytecode files): translates every shader, generates its GLSL (vertex shaders without a partner), prints counts per
 * shader version and the failures grouped by reason.   usage: sm3_check [-o glsl_dir] [-v] shaders.pak|shader.bin ... */
#include "sm3_glsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { uint32_t ver; unsigned n, bad; } g_ver[32]; static int g_nver;
static struct { char why[120]; unsigned n; uint32_t first; } g_why[64]; static int g_nwhy;
static const char* g_out; static int g_verbose; static unsigned g_tot, g_bad;
static void one(const uint32_t* tok, uint32_t ntok, uint32_t crc) {
    if (!ntok) return;
    uint32_t ver = tok[0]; int v = 0;
    for (; v < g_nver && g_ver[v].ver != ver; v++) {}
    if (v == g_nver && g_nver < 32) { g_ver[g_nver].ver = ver; g_nver++; }
    if (v < 32) g_ver[v].n++;
    g_tot++;
    Sm3 s; int r = sm3_translate(tok, ntok, &s);
    if (r) {
        g_bad++; if (v < 32) g_ver[v].bad++;
        char why[120]; snprintf(why, sizeof why, "%s", s.err);
        for (char* p = why; *p; p++) if (*p >= '0' && *p <= '9' && strstr(why, "token")) { *p = 0; break; }   /* group "at token N" */
        int k = 0; for (; k < g_nwhy && strcmp(g_why[k].why, why); k++) {}
        if (k == g_nwhy && g_nwhy < 64) { snprintf(g_why[k].why, sizeof g_why[k].why, "%s", why); g_why[k].first = crc; g_nwhy++; }
        if (k < 64) g_why[k].n++;
        if (g_verbose) fprintf(stderr, "%08x %s_%u_%u: %s\n", crc, (ver >> 16) == 0xFFFE ? "vs" : "ps", ver >> 8 & 0xFF, ver & 0xFF, s.err);
        return;
    }
    char* g = s.ps ? sm3_glsl_ps(&s) : sm3_glsl_vs(&s, NULL);
    if (g_out) {
        char path[1200]; snprintf(path, sizeof path, "%s/%s_%u_%u_%08x.glsl", g_out, s.ps ? "ps" : "vs", s.major, s.minor, crc);
        FILE* f = fopen(path, "w"); if (f) { fputs(g, f); fclose(f); }
    }
    free(g); sm3_free(&s);
}
static uint32_t crc32b(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}
int main(int argc, char** argv) {
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { g_out = argv[++i]; continue; }
        if (!strcmp(argv[i], "-v")) { g_verbose = 1; continue; }
        FILE* f = fopen(argv[i], "rb"); if (!f) { perror(argv[i]); continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* b = malloc((size_t)n + 8); if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0; fclose(f); nf++;
        if (n >= 8 && !memcmp(b, "ME1SHPK1", 8)) {
            long p = 8;
            while (p + 8 <= n) {
                uint32_t h[2]; memcpy(h, b + p, 8); p += 8;
                if (h[1] > (uint32_t)(n - p)) { fprintf(stderr, "%s: truncated record at %ld\n", argv[i], p); break; }
                uint32_t* t = malloc(h[1] + 4); memcpy(t, b + p, h[1]); one(t, h[1] / 4, h[0]); free(t); p += h[1];
            }
        } else if (n >= 4) {
            uint32_t* t = malloc((size_t)n + 4); memcpy(t, b, (size_t)n); one(t, (uint32_t)n / 4, crc32b(b, (size_t)n)); free(t);
        }
        free(b);
    }
    if (!nf) { fprintf(stderr, "usage: sm3_check [-o glsl_dir] [-v] shaders.pak|shader.bin ...\n"); return 2; }
    printf("%u shaders, %u not translated\n", g_tot, g_bad);
    for (int v = 0; v < g_nver; v++)
        printf("  %s_%u_%u: %u (%u not translated)\n", (g_ver[v].ver >> 16) == 0xFFFE ? "vs" : "ps", g_ver[v].ver >> 8 & 0xFF, g_ver[v].ver & 0xFF, g_ver[v].n, g_ver[v].bad);
    for (int k = 0; k < g_nwhy; k++) printf("  x%u %s (e.g. %08x)\n", g_why[k].n, g_why[k].why, g_why[k].first);
    return g_bad != 0;
}
