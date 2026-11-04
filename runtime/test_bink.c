/* Recompiled binkw32.dll (Bink 1.x video, rebased to 10400000) decoding a real .bik through the HLE file I/O and
 * threads, like the game: BinkOpen, the BINK header (size, frames, frame rate), BinkDoFrame + BinkCopyToBuffer
 * (BINKSURFACE32) + BinkNextFrame, the game's path (BinkGetFrameBuffersInfo + BinkRegisterFrameBuffers: YUV planes
 * in caller memory, as FBinkGearsMovieSupport does with its textures), BinkGoto, BinkClose; decode speed.
 * Frames are compared with ffmpeg's Bink decoder (PSNR) when a reference dump is given.
 * Audio: IDirectSound8 (hle_al.c) given to BinkSetSoundSystem(BinkOpenDirectSound), as the game does; 6 s of real-time
 * playback through the null sink, recorded and correlated with ffmpeg's decoded audio.
 * usage: test_bink <image dir with bink/> <dir with the .bik> <name.bik> [frames] [reference.bgra (ffmpeg -pix_fmt bgra)] [out dir for PPMs]
 *        [reference audio (ffmpeg -ac 2 -ar 44100 -f s16le)] */
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
MODDECL(bk)
#define MODINIT(mod, p, nm) do { (mod)->name = nm; (mod)->base = p##_base; (mod)->size = p##_image_size; (mod)->entry = p##_entry; \
    (mod)->funcs = p##_funcs; (mod)->nfuncs = p##_nfuncs; (mod)->imports = p##_imports; (mod)->nimports = p##_nimports; \
    (mod)->exports = p##_exports; rt_expect_image_crc(p##_image_crc); } while (0)
static Module bk;
static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t fn(const char* name, int n, ...) {
    uint32_t t = rt_find_export(&bk, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    if (!t) { fprintf(stderr, "FAIL: no export %s\n", name); g_fail++; return 0xFFFFFFFFu; }
    uint32_t sp = g_cpu.esp, r = rt_call(&g_cpu, t, n, a);
    if (g_cpu.esp != sp) { fprintf(stderr, "FAIL %s: esp %08x -> %08x (stdcall)\n", name, sp, g_cpu.esp); g_fail++; g_cpu.esp = sp; }
    return r;
}
static uint32_t gbuf(uint32_t n) { uint32_t a = ghalloc(n, 1); memset(M + a, 0, n); return a; }
static uint32_t gstr(const char* s) { uint32_t n = (uint32_t)strlen(s) + 1, g = ghalloc(n, 1); memcpy(M + g, s, n); return g; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double psnr_bgra(const uint8_t* a, const uint8_t* b, size_t px) {
    double se = 0; for (size_t i = 0; i < px; i++) for (int k = 0; k < 3; k++) { double d = (double)a[4 * i + k] - b[4 * i + k]; se += d * d; }
    double mse = se / (px * 3.0); return mse <= 1e-9 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}
static void save_ppm(const char* dir, const char* tag, int f, const uint8_t* bgra, int w, int h) {
    if (!dir) return;
    char p[1024]; snprintf(p, sizeof p, "%s/bink_%s_%04d.ppm", dir, tag, f);
    FILE* o = fopen(p, "wb"); if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t c[3] = { bgra[4 * i + 2], bgra[4 * i + 1], bgra[4 * i] }; fwrite(c, 1, 3, o); }
    fclose(o);
}
static const char* g_audio_ref; static char g_wav[1024];
static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <image dir> <movie dir> <name.bik> [frames] [reference.bgra] [ppm dir]\n", argv[0]); return 2; }
    int nfr = argc > 4 ? atoi(argv[4]) : 60; const char* ref = argc > 5 && strcmp(argv[5], "-") ? argv[5] : NULL; const char* ppm = argc > 6 ? argv[6] : NULL;
    if (argc > 7) {   /* ffmpeg s16le stereo 44.1 kHz of the movie: enables the DirectSound audio check */
        g_audio_ref = argv[7];
        snprintf(g_wav, sizeof g_wav, "%s/bink_audio.wav", ppm ? ppm : ".");
        setenv("ME1_AL_SINK", "null", 1); setenv("ME1_AL_WAV", g_wav, 1); setenv("ME1_AL_RATE", "44100", 1);
    }
    rt_init();
    hle_fs_init(argv[2], argv[2], NULL);
    MODINIT(&bk, bk, "binkw32.dll");
    { char p[1024]; snprintf(p, sizeof p, "%s/bink/image.bin", argv[1]); hle_register_module(&bk, "C:\\MassEffect\\Binaries\\binkw32.dll"); rt_load_module(&bk, p); }
    CHECK(hle_dll_attach(&bk) == 1, "binkw32 DllMain");
    char wp[600]; snprintf(wp, sizeof wp, "C:\\MassEffect\\%s", argv[3]);
    CHECK(fn("_BinkOpen@8", 2, gstr("C:\\MassEffect\\no_such_movie.bik"), 0u) == 0, "BinkOpen(missing file) -> NULL");
    double t0 = now();
    uint32_t h = fn("_BinkOpen@8", 2, gstr(wp), 0u);
    CHECK(h != 0, "BinkOpen(%s)", wp);
    if (!h) { uint32_t e = fn("_BinkGetError@0", 0); fprintf(stderr, "BinkGetError: %s\n", e ? (char*)(M + e) : "?"); return 1; }
    uint32_t W = rd32(h), H = rd32(h + 4), F = rd32(h + 8), fr = rd32(h + 20), frd = rd32(h + 24);
    fprintf(stderr, "[bink] %ux%u, %u frames, %u/%u fps, opened in %.1f ms\n", W, H, F, fr, frd, (now() - t0) * 1e3);
    CHECK(W == 1280 && H == 720, "size %ux%u", W, H);
    CHECK(F > 100 && fr && frd && fabs((double)fr / frd - 29.97) < 0.1, "frames %u, rate %u/%u", F, fr, frd);
    CHECK(rd32(h + 12) == 1, "FrameNum starts at 1 (%u)", rd32(h + 12));
    if (nfr > (int)F) nfr = (int)F;
    /* 1) BinkCopyToBuffer into a 32-bit BGRX buffer */
    uint32_t pitch = W * 4, dst = gbuf(pitch * H);
    FILE* rf = ref ? fopen(ref, "rb") : NULL; uint8_t* refb = rf ? malloc((size_t)pitch * H) : NULL;
    double tdec = 0, tcopy = 0, minp = 99, sump = 0; int nref = 0;
    for (int f = 0; f < nfr; f++) {
        double a = now();
        fn("_BinkDoFrame@4", 1, h);
        double b = now();
        fn("_BinkCopyToBuffer@28", 7, h, dst, pitch, H, 0u, 0u, 3u /* BINKSURFACE32 */);
        double c = now();
        tdec += b - a; tcopy += c - b;
        if (rf && fread(refb, 1, (size_t)pitch * H, rf) == (size_t)pitch * H) {
            double p = psnr_bgra(M + dst, refb, (size_t)W * H); if (p < minp) minp = p; sump += p; nref++;
            if (p < 30 && ppm) { save_ppm(ppm, "bad", f, M + dst, (int)W, (int)H); save_ppm(ppm, "ref", f, refb, (int)W, (int)H); }
        }
        if (f == 0 || f == nfr - 1 || f % 100 == 50) save_ppm(ppm, "rgb", f, M + dst, (int)W, (int)H);
        if (f + 1 < nfr) fn("_BinkNextFrame@4", 1, h);
    }
    fprintf(stderr, "[bink] %d frames: decode %.2f ms/frame, copy (BINKSURFACE32) %.2f ms/frame -> %.1f fps\n", nfr, tdec * 1e3 / nfr,
            tcopy * 1e3 / nfr, nfr / (tdec + tcopy));
    if (nref) { fprintf(stderr, "[bink] vs ffmpeg: PSNR min %.1f dB, mean %.1f dB over %d frames\n", minp, sump / nref, nref);
                CHECK(minp > 30, "decoded frames match ffmpeg's Bink decoder (min PSNR %.1f dB)", minp); }
    uint32_t mean = 0; for (uint32_t i = 0; i < W * H; i += 97) mean += M[dst + 4 * i + 1];
    CHECK(mean > 0, "last frame is not black");
    CHECK((int)rd32(h + 12) == nfr, "FrameNum %u after %d frames", rd32(h + 12), nfr);
    fn("_BinkClose@4", 1, h);
    /* 2) the game's path: frame buffers in caller memory (BINKFRAMEBUFFERS), YUV planes decoded in place */
    h = fn("_BinkOpen@8", 2, gstr(wp), 0x00000400u /* BINKNOFRAMEBUFFERS */);
    CHECK(h != 0, "BinkOpen(BINKNOFRAMEBUFFERS)");
    if (h) {
        /* BINKFRAMEBUFFERS: TotalFrames, YABufferWidth, YABufferHeight, cRcBBufferWidth, cRcBBufferHeight, FrameNum,
         * Frames[2] x { YPlane, cRPlane, cBPlane, APlane } x { Allocate, Buffer, BufferPitch } */
        uint32_t fb = gbuf(6 * 4 + 2 * 4 * 12);
        fn("_BinkGetFrameBuffersInfo@8", 2, h, fb);
        uint32_t tf = rd32(fb), yw = rd32(fb + 4), yh = rd32(fb + 8), cw = rd32(fb + 12), ch = rd32(fb + 16);
        fprintf(stderr, "[bink] frame buffers: %u, Y %ux%u, chroma %ux%u\n", tf, yw, yh, cw, ch);
        CHECK(tf >= 1 && tf <= 2 && yw >= W && yh >= H && cw * 2 >= W && ch * 2 >= H, "BinkGetFrameBuffersInfo");
        for (uint32_t i = 0; i < tf && i < 2; i++) for (int pl = 0; pl < 4; pl++) {
            uint32_t pd = fb + 24 + i * 48 + (uint32_t)pl * 12;
            if (!rd32(pd)) continue;   /* Allocate */
            uint32_t pw = pl == 0 || pl == 3 ? yw : cw, ph = pl == 0 || pl == 3 ? yh : ch;
            wr32(pd + 4, gbuf(pw * ph)); wr32(pd + 8, pw);
        }
        fn("_BinkRegisterFrameBuffers@8", 2, h, fb);
        double a = now(); int n2 = nfr < 30 ? nfr : 30;
        for (int f = 0; f < n2; f++) { fn("_BinkDoFrame@4", 1, h); if (f + 1 < n2) fn("_BinkNextFrame@4", 1, h); }
        double dt = now() - a;
        uint32_t cur = rd32(fb + 20);   /* FrameNum: the buffer set holding the current frame */
        uint32_t base = fb + 24 + (cur < 2 ? cur : 0) * 48;
        uint32_t Y = rd32(base + 4), Yp = rd32(base + 8), Cr = rd32(base + 16), Crp = rd32(base + 20), Cb = rd32(base + 28), Cbp = rd32(base + 32);
        fprintf(stderr, "[bink] YUV path: %d frames in %.2f ms/frame; current set %u: Y %08x/%u cR %08x/%u cB %08x/%u\n", n2, dt * 1e3 / n2, cur, Y, Yp, Cr, Crp, Cb, Cbp);
        CHECK(Y && Cr && Cb, "YUV planes");
        if (Y && Cr && Cb) {
            uint8_t* rgb = malloc((size_t)W * H * 4);
            for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
                int yy = M[Y + y * Yp + x] - 16, u = M[Cb + (y / 2) * Cbp + x / 2] - 128, v = M[Cr + (y / 2) * Crp + x / 2] - 128;
                uint8_t* o = rgb + 4 * ((size_t)y * W + x);
                o[2] = clamp8((298 * yy + 409 * v + 128) >> 8); o[1] = clamp8((298 * yy - 100 * u - 208 * v + 128) >> 8); o[0] = clamp8((298 * yy + 516 * u + 128) >> 8); o[3] = 255;
            }
            save_ppm(ppm, "yuv", n2 - 1, rgb, (int)W, (int)H);
            /* the same frame decoded by ffmpeg (BT.601 limited range, like the engine's YUV shader) */
            FILE* rf2 = ref ? fopen(ref, "rb") : NULL;
            if (rf2) {
                uint8_t* r2 = malloc((size_t)W * H * 4);
                if (fseek(rf2, (long)((size_t)(n2 - 1) * W * H * 4), SEEK_SET) == 0 && fread(r2, 4, (size_t)W * H, rf2) == (size_t)W * H) {
                    double p = psnr_bgra(rgb, r2, (size_t)W * H);
                    fprintf(stderr, "[bink] YUV planes (BT.601) vs ffmpeg frame %d: PSNR %.1f dB\n", n2 - 1, p);
                    CHECK(p > 28, "YUV planes hold the right picture (PSNR %.1f dB)", p);
                }
                free(r2); fclose(rf2);
            }
            free(rgb);
        }
        fn("_BinkClose@4", 1, h);
    }
    /* 3) movie audio as in the game: IDirectSound8 (hle_al.c) -> BinkSetSoundSystem(BinkOpenDirectSound, ds); real-time
     * playback with the null sink recording the mix (ME1_AL_WAV), compared with ffmpeg's decoded audio */
    if (g_audio_ref) {
        uint32_t a3[3] = { 0, gbuf(4), 0 };
        uint32_t hr = hle_call("dsound.dll", "#11", 3, a3), ds = rd32(a3[1]);
        CHECK(hr == 0 && ds, "DirectSoundCreate8 -> %08x", hr);
        if (ds) {
            uint32_t vt = rd32(ds), sc[3] = { ds, 0x00010012u, 2 /* DSSCL_PRIORITY */ };
            CHECK(rt_call(&g_cpu, rd32(vt + 6 * 4), 3, sc) == 0, "SetCooperativeLevel");
            uint32_t ok = fn("_BinkSetSoundSystem@8", 2, rt_find_export(&bk, "_BinkOpenDirectSound@4"), ds);
            CHECK(ok != 0, "BinkSetSoundSystem(BinkOpenDirectSound, ds)");
            h = fn("_BinkOpen@8", 2, gstr(wp), 0u);
            CHECK(h != 0, "BinkOpen with DirectSound audio");
            if (h) {
                int want = 180; double a = now(), lim = a + 20; int shown = 0, waits = 0;
                while (rd32(h + 12) < (uint32_t)want && now() < lim) {
                    if (fn("_BinkWait@4", 1, h)) { struct timespec t = { 0, 1000000 }; nanosleep(&t, NULL); waits++; continue; }
                    fn("_BinkDoFrame@4", 1, h); fn("_BinkNextFrame@4", 1, h); shown++;
                }
                double dt = now() - a;
                fprintf(stderr, "[bink] real-time playback with DirectSound: %d frames in %.2f s (%d waits)\n", shown, dt, waits);
                CHECK(shown >= want - 1 && dt > want / 30.0 * 0.85 && dt < want / 30.0 * 1.5, "frames paced in real time (%.2f s)", dt);
                fn("_BinkClose@4", 1, h);
            }
            rt_call(&g_cpu, rd32(vt + 2 * 4), 1, &ds);
            hle_al_report();
            /* compare the recording with ffmpeg: align at the onset, then the best lag around it */
            FILE* w = fopen(g_wav, "rb"); FILE* r = fopen(g_audio_ref, "rb");
            if (w && r) {
                fseek(w, 0, SEEK_END); long wn = (ftell(w) - 44) / 2; fseek(w, 44, SEEK_SET);
                fseek(r, 0, SEEK_END); long rn = ftell(r) / 2; fseek(r, 0, SEEK_SET);
                int16_t* wb = malloc((size_t)(wn > 0 ? wn : 1) * 2); int16_t* rb = malloc((size_t)rn * 2);
                wn = (long)fread(wb, 2, (size_t)(wn > 0 ? wn : 0), w); rn = (long)fread(rb, 2, (size_t)rn, r);
                long ow = -1, orf = -1;
                for (long i = 0; i < wn; i++) if (abs(wb[i]) > 200) { ow = i & ~1L; break; }
                for (long i = 0; i < rn; i++) if (abs(rb[i]) > 200) { orf = i & ~1L; break; }
                fprintf(stderr, "[bink] audio onset: recording %.3f s, ffmpeg %.3f s\n", ow / 88200.0, orf / 88200.0);
                CHECK(ow >= 0 && orf >= 0, "movie audio was played");
                if (ow >= 0 && orf >= 0) {
                    double best = -2; long bl = 0; long win = 88200;   /* 1 s stereo */
                    for (long lag = -4000; lag <= 4000; lag += 2) {
                        long s0 = ow + lag + 44100, s1 = orf + 44100;
                        if (s0 < 0 || s0 + win > wn || s1 + win > rn) continue;
                        double xy = 0, xx = 0, yy = 0;
                        for (long i = 0; i < win; i++) { double x = wb[s0 + i], y = rb[s1 + i]; xy += x * y; xx += x * x; yy += y * y; }
                        double cc = xx > 0 && yy > 0 ? xy / sqrt(xx * yy) : 0;
                        if (cc > best) { best = cc; bl = lag; }
                    }
                    fprintf(stderr, "[bink] audio vs ffmpeg: correlation %.4f at lag %ld samples\n", best, bl / 2);
                    CHECK(best > 0.95, "movie audio matches ffmpeg (correlation %.3f)", best);
                }
                free(wb); free(rb);
            }
            if (w) fclose(w);
            if (r) fclose(r);
        }
    }
    fprintf(stderr, "test_bink: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
