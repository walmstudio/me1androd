/* Recompiled Ogg Vorbis DLLs of the game (ogg.dll, vorbis.dll, vorbisfile.dll, rebased to 10100000/10200000/10300000)
 * decoding real .ogg files through the recompiled msvcr80 file I/O, as ISACT does: ov_open_callbacks / ov_open,
 * ov_info, ov_comment, ov_pcm_total, ov_read (16-bit LE signed), ov_pcm_seek, ov_clear. The PCM is compared with a
 * reference decode by ffmpeg's libvorbis decoder (same float -> int16 conversion), plus decode speed (x realtime).
 * usage: test_vorbis <msvcr80.dll|image.bin> <image dir with ogg/ vorbis/ vorbisfile/> <data dir>
 * data dir (made by `make test-vorbis`): <name>.ogg + <name>.raw (s16le reference) + <name>.txt ("channels rate frames") */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#define MODDECL(p) extern const FuncEntry p##_funcs[]; extern const unsigned p##_nfuncs; \
    extern const ImportEntry p##_imports[]; extern const unsigned p##_nimports; \
    extern const ExportEntry p##_exports[]; extern const uint32_t p##_base, p##_entry, p##_image_size, p##_image_crc;
MODDECL(crt) MODDECL(ogg) MODDECL(vb) MODDECL(vf)
#define MODINIT(mod, p, nm) do { (mod)->name = nm; (mod)->base = p##_base; (mod)->size = p##_image_size; (mod)->entry = p##_entry; \
    (mod)->funcs = p##_funcs; (mod)->nfuncs = p##_nfuncs; (mod)->imports = p##_imports; (mod)->nimports = p##_nimports; \
    (mod)->exports = p##_exports; rt_expect_image_crc(p##_image_crc); } while (0)
static Module crt, ogg, vb, vf;
static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t call(uint32_t t, int n, const uint32_t* a, const char* what) {
    uint32_t sp = g_cpu.esp, r = rt_call(&g_cpu, t, n, a);
    if (g_cpu.esp != sp) { fprintf(stderr, "FAIL %s: esp\n", what); g_fail++; g_cpu.esp = sp; }
    return r;
}
static uint32_t fn(Module* m, const char* name, int n, ...) {
    uint32_t t = rt_find_export(m, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    if (!t) { fprintf(stderr, "FAIL: no export %s\n", name); g_fail++; return 0xFFFFFFFFu; }
    return call(t, n, a, name);
}
static uint64_t ret64(uint32_t eax) { return (uint64_t)eax | (uint64_t)g_cpu.edx << 32; }
static uint32_t gbuf(uint32_t n) { uint32_t a = ghalloc(n, 1); memset(M + a, 0, n); return a; }
static uint32_t gstr(const char* s) { uint32_t n = (uint32_t)strlen(s) + 1, g = ghalloc(n, 1); memcpy(M + g, s, n); return g; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void load(Module* m, const char* dir, const char* sub, const char* winpath) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s/image.bin", dir, sub);
    hle_register_module(m, winpath); rt_load_module(m, p);
}
static uint32_t gopen(const char* name) {
    char w[300]; snprintf(w, sizeof w, "C:\\MassEffect\\%s", name);
    uint32_t a = gstr(w), md = gstr("rb"), f = fn(&crt, "fopen", 2, a, md);
    ghfree(a); ghfree(md); return f;
}
static int16_t* read_file(const char* dir, const char* name, size_t* n) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE* f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    int16_t* b = malloc((size_t)sz + 2); *n = fread(b, 1, (size_t)sz, f) / 2; fclose(f); return b;
}
/* this vorbisfile.dll build takes sizeof(OggVorbis_File) as an extra last argument of ov_open/ov_open_callbacks
 * (ABI check: anything else -> -139) */
#define OVF_SIZE 0x2d8u
/* open with the game's way (callbacks = crt fread/_fseeki64/fclose/ftell) or ov_open(FILE*) */
static uint32_t ov_open_file(const char* name, uint32_t ovf, int use_cb, int* err) {
    uint32_t f = gopen(name);
    if (!f) { *err = -9999; return 0; }
    if (use_cb) {
        uint32_t cb[4] = { rt_find_export(&crt, "fread"), rt_find_export(&crt, "_fseeki64"), rt_find_export(&crt, "fclose"),
                           rt_find_export(&crt, "ftell") };
        *err = (int)fn(&vf, "ov_open_callbacks", 9, f, ovf, 0u, 0u, cb[0], cb[1], cb[2], cb[3], OVF_SIZE);
    } else *err = (int)fn(&vf, "ov_open", 5, f, ovf, 0u, 0u, OVF_SIZE);
    return f;
}
/* decode everything with ov_read: this build has no endianness/word-size arguments (always 16-bit LE):
 * ov_read(vf, buffer, length, sgned, int* bitstream) -> bytes */
static int16_t* decode_all(uint32_t ovf, size_t* frames, int ch, int chunk, double* secs) {
    size_t cap = 1 << 20, n = 0; int16_t* out = malloc(cap * 2);
    uint32_t buf = gbuf(65536), bs = gbuf(4); int bad = 0;
    double t0 = now();
    for (;;) {
        int r = (int)fn(&vf, "ov_read", 5, ovf, buf, (uint32_t)chunk, 1u, bs);
        if (r == 0) break;
        if (r < 0) { if (bad++ < 3) fprintf(stderr, "ov_read -> %d (hole)\n", r); if (bad > 20) break; continue; }
        if (n + (size_t)r / 2 > cap) { cap *= 2; out = realloc(out, cap * 2); }
        memcpy(out + n, M + buf, (size_t)r); n += (size_t)r / 2;
    }
    *secs = now() - t0;
    CHECK(bad == 0, "ov_read errors: %d", bad);
    CHECK(rd32(bs) == 0, "bitstream %u", rd32(bs));
    ghfree(buf); ghfree(bs);
    *frames = n / (size_t)ch; return out;
}
/* ov_read converts 8-sample groups with cvtps2pi (round to nearest) and the rest with cvttss2si (truncate), so the
 * same position can differ by 1 LSB between read sizes - in the original DLL too */
static int maxdiff(const int16_t* a, const int16_t* b, size_t n, size_t* nd) {
    int m = 0; *nd = 0;
    for (size_t i = 0; i < n; i++) { int d = abs((int)a[i] - (int)b[i]); if (d) ++*nd; if (d > m) m = d; }
    return m;
}
static void compare(const char* what, const int16_t* a, const int16_t* ref, size_t n, int tol) {
    int maxd = 0; double se = 0, sr = 0; size_t over = 0;
    for (size_t i = 0; i < n; i++) {
        int d = abs((int)a[i] - (int)ref[i]); if (d > maxd) maxd = d; if (d > tol) over++;
        se += (double)d * d; sr += (double)ref[i] * ref[i];
    }
    double snr = se > 0 ? 10 * log10(sr / se) : 999;
    printf("  %s: %zu samples, max |diff| %d LSB, %zu over %d, SNR vs reference %.1f dB\n", what, n, maxd, over, tol, snr);
    CHECK(maxd <= tol && snr > 70, "%s: max diff %d, SNR %.1f dB", what, maxd, snr);
}
static void test_file(const char* dir, const char* base, int use_cb) {
    char nm[256], rn[256], tn[256]; snprintf(nm, sizeof nm, "%s.ogg", base); snprintf(rn, sizeof rn, "%s.raw", base);
    snprintf(tn, sizeof tn, "%s/%s.txt", dir, base);
    int ech = 0; long erate = 0, eframes = 0; FILE* t = fopen(tn, "r");
    if (!t || fscanf(t, "%d %ld %ld", &ech, &erate, &eframes) != 3) { CHECK(0, "%s: missing", tn); if (t) fclose(t); return; }
    fclose(t);
    size_t nref = 0; int16_t* ref = read_file(dir, rn, &nref);
    CHECK(ref != NULL, "%s: reference missing", rn); if (!ref) return;
    printf("%s (%s): %d ch, %ld Hz, %ld frames\n", nm, use_cb ? "ov_open_callbacks + crt fread/_fseeki64/fclose/ftell" : "ov_open", ech, erate, eframes);
    uint32_t ovf = gbuf(4096); int err;
    ov_open_file(nm, ovf, use_cb, &err);
    CHECK(err == 0, "%s: ov_open -> %d", nm, err); if (err) { free(ref); return; }
    uint32_t vi = fn(&vf, "ov_info", 2, ovf, 0xFFFFFFFFu);
    CHECK(vi && (int)rd32(vi + 4) == ech && (long)rd32(vi + 8) == erate, "ov_info: ch %d rate %d", vi ? (int)rd32(vi + 4) : -1, vi ? (int)rd32(vi + 8) : -1);
    CHECK(fn(&vf, "ov_seekable", 1, ovf) == 1, "ov_seekable");
    CHECK(fn(&vf, "ov_streams", 1, ovf) == 1, "ov_streams");
    int64_t total = (int64_t)ret64(fn(&vf, "ov_pcm_total", 2, ovf, 0xFFFFFFFFu));
    CHECK(total == eframes, "ov_pcm_total %lld, expected %ld", (long long)total, eframes);
    uint32_t vc = fn(&vf, "ov_comment", 2, ovf, 0xFFFFFFFFu);
    int found = 0;
    if (vc) {
        /* vorbis_comment of this build: {char** user_comments; int* lengths; int comments; char vendor[64]} (0x4c) */
        uint32_t uc = rd32(vc), ncom = rd32(vc + 8);
        for (uint32_t i = 0; i < ncom && i < 64; i++) if (strstr(GSTR(rd32(uc + 4 * i)), "ME1 test tone")) found = 1;
        printf("  vendor \"%.63s\", %u comments\n", GSTR(vc + 12), ncom);
        CHECK(!strncmp(GSTR(vc + 12), "Lavf", 4), "vendor (ffmpeg's muxer writes Lavf...)");
    }
    CHECK(found, "ov_comment: title comment not found");
    size_t frames; double secs;
    int16_t* pcm = decode_all(ovf, &frames, ech, 4096, &secs);
    double dur = (double)frames / (double)erate;
    printf("  decoded %zu frames (%.2f s of audio) in %.3f s: %.1fx realtime\n", frames, dur, secs, dur / (secs > 0 ? secs : 1e-9));
    CHECK((int64_t)frames == total, "decoded %zu frames, ov_pcm_total %lld", frames, (long long)total);
    size_t nc = frames * (size_t)ech < nref ? frames * (size_t)ech : nref;
    if (nref != frames * (size_t)ech) printf("  (reference has %zu frames)\n", nref / (size_t)ech);
    compare("full decode", pcm, ref, nc, 3);
    /* small odd reads (the stream mixer asks for what fits its ring buffer) give the same samples */
    CHECK(fn(&vf, "ov_raw_seek", 3, ovf, 0u, 0u) == 0, "ov_raw_seek 0");
    size_t f2; double s2; int16_t* pcm2 = decode_all(ovf, &f2, ech, 1002, &s2);
    { size_t nd = 0; int md = maxdiff(pcm, pcm2, (f2 < frames ? f2 : frames) * (size_t)ech, &nd);
      printf("  re-decode with 1002-byte reads: %zu frames, %zu samples differ by <= %d LSB (vector/scalar rounding)\n", f2, nd, md);
      CHECK(f2 == frames && md <= 1 && nd < frames / 50, "re-decode with 1002-byte reads: %zu frames, max diff %d (%zu)", f2, md, nd); }
    free(pcm2);
    /* sample-accurate seek */
    const int64_t pos[3] = { eframes / 3, eframes / 2 + 777, eframes - 1500 };
    for (int k = 0; k < 3; k++) {
        int r = (int)fn(&vf, "ov_pcm_seek", 3, ovf, (uint32_t)pos[k], (uint32_t)(pos[k] >> 32));
        int64_t tell = (int64_t)ret64(fn(&vf, "ov_pcm_tell", 1, ovf));
        CHECK(r == 0 && tell == pos[k], "ov_pcm_seek(%lld) -> %d, tell %lld", (long long)pos[k], r, (long long)tell);
        uint32_t buf = gbuf(8192), bs = gbuf(4); size_t got = 0; int16_t tmp[4096];
        while (got < 1024 * (size_t)ech) {
            int n = (int)fn(&vf, "ov_read", 5, ovf, buf, 2048u, 1u, bs);
            if (n <= 0) break;
            size_t take = (size_t)n / 2; if (got + take > 4096) take = 4096 - got;
            memcpy(tmp + got, M + buf, take * 2); got += take;
        }
        size_t want = 1024 * (size_t)ech; if ((size_t)(eframes - pos[k]) * (size_t)ech < want) want = (size_t)(eframes - pos[k]) * (size_t)ech;
        size_t nd = 0; int md = maxdiff(tmp, pcm + (size_t)pos[k] * (size_t)ech, want, &nd);
        CHECK(got >= want && md <= 1, "after ov_pcm_seek(%lld): max diff %d from the linear decode", (long long)pos[k], md);
        ghfree(buf); ghfree(bs);
    }
    fn(&vf, "ov_time_total", 2, ovf, 0xFFFFFFFFu);   /* double result in st(0) */
    if (!(g_cpu.fempty & (1u << g_cpu.ftop))) {
        double st0 = g_cpu.st[g_cpu.ftop]; fpop(&g_cpu);
        CHECK(fabs(st0 - (double)eframes / erate) < 1e-6, "ov_time_total %f", st0);
    } else CHECK(0, "ov_time_total: nothing on the x87 stack");
    CHECK(fn(&vf, "ov_clear", 1, ovf) == 0, "ov_clear");
    free(pcm); free(ref); ghfree(ovf);
}
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <msvcr80.dll|image.bin> <image dir> <data dir>\n", argv[0]); return 2; }
    rt_init();
    hle_fs_init(argv[3], argv[3], NULL);
    MODINIT(&crt, crt, "msvcr80.dll"); hle_register_module(&crt, "C:\\MassEffect\\Binaries\\msvcr80.dll"); rt_load_module(&crt, argv[1]);
    MODINIT(&ogg, ogg, "ogg.dll"); load(&ogg, argv[2], "ogg", "C:\\MassEffect\\Binaries\\ogg.dll");
    MODINIT(&vb, vb, "vorbis.dll"); load(&vb, argv[2], "vorbis", "C:\\MassEffect\\Binaries\\vorbis.dll");
    MODINIT(&vf, vf, "vorbisfile.dll"); load(&vf, argv[2], "vorbisfile", "C:\\MassEffect\\Binaries\\vorbisfile.dll");
    CHECK(hle_dll_attach(&crt) == 1, "msvcr80 DllMain");
    CHECK(hle_dll_attach(&ogg) == 1 && hle_dll_attach(&vb) == 1 && hle_dll_attach(&vf) == 1, "ogg/vorbis/vorbisfile DllMain");
    CHECK(hle_load_library("vorbisfile.dll") == vf_base && vf_base == 0x10300000u, "LoadLibraryA(vorbisfile.dll)");
    CHECK(hle_get_proc(vf_base, "ov_read", 0) == rt_find_export(&vf, "ov_read"), "GetProcAddress(ov_read)");
    test_file(argv[3], "stereo44", 1);
    test_file(argv[3], "mono22", 1);
    test_file(argv[3], "stereo44", 0);
    test_file(argv[3], "q0_48k", 1);
    /* not an Ogg stream: a clean error, no crash; wrong struct size: -139 */
    { uint32_t ovf = gbuf(4096); int err; ov_open_file("notogg.ogg", ovf, 1, &err);
      CHECK(err < 0 && err != -139, "ov_open_callbacks(garbage) -> %d", err);
      printf("garbage file: ov_open_callbacks -> %d\n", err);
      uint32_t f = gopen("mono22.ogg");
      CHECK((int)fn(&vf, "ov_open", 5, f, ovf, 0u, 0u, 0x2d0u) == -139, "ov_open with a wrong sizeof");
      fn(&crt, "fclose", 1, f); }
    printf("test_vorbis: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
