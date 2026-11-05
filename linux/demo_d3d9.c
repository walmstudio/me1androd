/* me1-demo: the Linux front end (me1_linux.c) with a small D3D9 + OpenAL + input program in place of MassEffect.exe.
 * It checks the whole desktop path without the game files: SDL2 window -> me1_platform -> EGL/GLES or Vulkan presenter,
 * keyboard / mouse / gamepad -> Win32 + XInput HLE (read back with GetAsyncKeyState / GetCursorPos / XInputGetState,
 * exactly as the game does), SDL audio -> OpenAL HLE mixer. Everything goes through the guest-visible thunks and COM
 * vtables (like runtime/test_gl.c), so it exercises the same code as the game.
 *   me1-demo --game <any dir with Binaries/> [front-end options]      ESC: quit
 * env: ME1_DEMO_SECONDS=N (quit after N s), ME1_DEMO_SHOT=N (print "[demo] frame N" markers for scripted screenshots),
 *      ME1_DEMO_LOAD=N (wait N s after CreateDevice before the first Present: shows the loading screen, as the game's
 *      start-up does). */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

const char* me1gl_renderer(void);
void me1_perf(int64_t o[4]);
static uint32_t api(const char* dll, const char* name, int n, ...) {
    uint32_t t = hle_resolve_impl(dll, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    if (!t) { fprintf(stderr, "[demo] %s!%s not implemented\n", dll, name); return 0; }
    return rt_call(&g_cpu, t, n, a);
}
static uint32_t dev, out;
static uint32_t com(uint32_t o, int k, int n, ...) {
    uint32_t a[16]; va_list ap; va_start(ap, n);
    a[0] = o; for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return rt_call(&g_cpu, rd32(rd32(o) + 4 * k), n + 1, a);
}
static uint32_t gbuf(uint32_t n) { uint32_t a = ghalloc(n, 1); memset(M + a, 0, n); return a; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ---- shaders (D3D9 bytecode, as in test_gl.c) ---- */
enum { R_TEMP = 0, R_IN = 1, R_CONST = 2, R_OUT = 6, R_COLOROUT = 8, R_SAMP = 10 };
#define RTY(t) ((((uint32_t)(t) & 7) << 28) | ((((uint32_t)(t) >> 3) & 3) << 11))
#define DST(t, n) (0x800F0000u | RTY(t) | (n))
#define SRC(t, n) (0x80E40000u | RTY(t) | (n))
#define OP(o, len) ((uint32_t)(o) | ((uint32_t)(len) << 24))
#define DCL(usage, idx, t, n) OP(0x1F, 2), 0x80000000u | (usage) | ((uint32_t)(idx) << 16), DST(t, n)
#define DCL2D(n) OP(0x1F, 2), 0x90000000u, DST(R_SAMP, n)
#define MOV(d, s) OP(0x01, 2), d, s
#define MUL(d, a, b) OP(0x05, 3), d, a, b
#define TEXLD(d, c, s) OP(0x42, 3), d, c, s
#define END 0x0000FFFFu
enum { U_POS = 0, U_TC = 5, U_COLOR = 10 };
static uint32_t shader(int k, const uint32_t* t, int n) {
    uint32_t c = gbuf(4 * n); for (int i = 0; i < n; i++) wr32(c + 4 * i, t[i]);
    if (com(dev, k, 2, c, out)) { fprintf(stderr, "[demo] shader creation failed\n"); return 0; }
    return rd32(out);
}

/* ---- vertex batches: float3 pos (clip), float2 uv, D3DCOLOR ---- */
#define VMAX 60000
static uint32_t vb; static int nv;
static float g_aspect = 16.0f / 9.0f;
static void v(float x, float y, float u, float t, uint32_t c) {
    if (nv >= VMAX) return;
    uint32_t a = vb + 24u * (uint32_t)nv++;
    wr32(a, fb(x)); wr32(a + 4, fb(y)); wr32(a + 8, fb(0.5f)); wr32(a + 12, fb(u)); wr32(a + 16, fb(t)); wr32(a + 20, c);
}
static void flush(void) { if (nv >= 3) com(dev, 83, 4, 4u /* TRIANGLELIST */, (uint32_t)(nv / 3), vb, 24u); nv = 0; }
static void rect(float x0, float y0, float x1, float y1, uint32_t c) {   /* clip coords */
    v(x0, y0, 0, 0, c); v(x1, y0, 1, 0, c); v(x0, y1, 0, 1, c); v(x1, y0, 1, 0, c); v(x1, y1, 1, 1, c); v(x0, y1, 0, 1, c);
}
/* pixel rect on a 1280x720 canvas */
static void prect(float x, float y, float w, float h, uint32_t c) { rect(x / 640 - 1, 1 - y / 360, (x + w) / 640 - 1, 1 - (y + h) / 360, c); }
static void disc(float cx, float cy, float r, uint32_t cin, uint32_t cout, int seg, float rot, float tex) {
    for (int i = 0; i < seg; i++) {
        float a0 = rot + 6.2831853f * i / seg, a1 = rot + 6.2831853f * (i + 1) / seg;
        v(cx, cy, 0.5f * tex, 0.5f * tex, cin);
        v(cx + r * cosf(a0) / g_aspect, cy + r * sinf(a0), (0.5f + 0.5f * cosf(a0)) * tex, (0.5f + 0.5f * sinf(a0)) * tex, cout);
        v(cx + r * cosf(a1) / g_aspect, cy + r * sinf(a1), (0.5f + 0.5f * cosf(a1)) * tex, (0.5f + 0.5f * sinf(a1)) * tex, cout);
    }
}
static void ring(float cx, float cy, float r0, float r1, uint32_t c0, uint32_t c1, int seg, float tilt) {
    for (int i = 0; i < seg; i++) {
        float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
        float x00 = cx + r0 * cosf(a0) / g_aspect, y00 = cy + r0 * sinf(a0) * tilt, x01 = cx + r1 * cosf(a0) / g_aspect, y01 = cy + r1 * sinf(a0) * tilt;
        float x10 = cx + r0 * cosf(a1) / g_aspect, y10 = cy + r0 * sinf(a1) * tilt, x11 = cx + r1 * cosf(a1) / g_aspect, y11 = cy + r1 * sinf(a1) * tilt;
        v(x00, y00, 0, 0, c0); v(x01, y01, 0, 0, c1); v(x10, y10, 0, 0, c0);
        v(x01, y01, 0, 0, c1); v(x11, y11, 0, 0, c1); v(x10, y10, 0, 0, c0);
    }
}

/* ---- 5x7 pixel font ---- */
static const char* glyph(char ch) {
    switch (ch) {
    case 'A': return " ### #   ##   #######   ##   ##   #";
    case 'B': return "#### #   ##   ##### #   ##   ##### ";
    case 'C': return " ### #   ##    #    #    #   # ### ";
    case 'D': return "#### #   ##   ##   ##   ##   ##### ";
    case 'E': return "######    #    #### #    #    #####";
    case 'F': return "######    #    #### #    #    #    ";
    case 'G': return " ### #   ##    # ####   ##   # ####";
    case 'H': return "#   ##   ##   #######   ##   ##   #";
    case 'I': return " ###   #    #    #    #    #   ### ";
    case 'J': return "  ###   #    #    #    # #  #  ##  ";
    case 'K': return "#   ##  # # #  ##   # #  #  # #   #";
    case 'L': return "#    #    #    #    #    #    #####";
    case 'M': return "#   ### ### # ## # ##   ##   ##   #";
    case 'N': return "#   ##   ###  ## # ##  ###   ##   #";
    case 'O': return " ### #   ##   ##   ##   ##   # ### ";
    case 'P': return "#### #   ##   ##### #    #    #    ";
    case 'Q': return " ### #   ##   ##   ## # ##  #  ## #";
    case 'R': return "#### #   ##   ##### # #  #  # #   #";
    case 'S': return " #####    #     ###     #    ##### ";
    case 'T': return "#####  #    #    #    #    #    #  ";
    case 'U': return "#   ##   ##   ##   ##   ##   # ### ";
    case 'V': return "#   ##   ##   ##   ##   # # #   #  ";
    case 'W': return "#   ##   ##   ## # ## # ## # # # # ";
    case 'X': return "#   ##   # # #   #   # # #   ##   #";
    case 'Y': return "#   ##   # # #   #    #    #    #  ";
    case 'Z': return "#####    #   #   #   #   #    #####";
    case '0': return " ### #   ##  ### # ###  ##   # ### ";
    case '1': return "  #   ##    #    #    #    #   ### ";
    case '2': return " ### #   #    #   #   #   #   #####";
    case '3': return "#####   #   #     #     ##   # ### ";
    case '4': return "   #   ##  # # #  # #####   #    # ";
    case '5': return "######    ####     #    ##   # ### ";
    case '6': return "  ##  #   #    #### #   ##   # ### ";
    case '7': return "#####    #   #   #   #    #    #   ";
    case '8': return " ### #   ##   # ### #   ##   # ### ";
    case '9': return " ### #   ##   # ####    #   #  ##  ";
    case '.': return "                          ##   ##  ";
    case ':': return "      ##   ##        ##   ##       ";
    case '-': return "                ###                ";
    case '/': return "    #    #   #   #   #   #    #    ";
    case '(': return "   #   #   #    #    #     #     # ";
    case ')': return " #     #     #    #    #   #   #   ";
    case '+': return "       #    #  #####  #    #       ";
    case '%': return "##   ##  #   #   #   #   #  ##   ##";
    case '=': return "          #####     #####          ";
    case ',': return "                     ##    #   #   ";
    default: return NULL;
    }
}
static void text(float x, float y, float px, const char* s, uint32_t c) {
    for (; *s; s++, x += 6 * px) {
        char ch = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        const char* g = glyph(ch); if (!g) continue;
        for (int r = 0; r < 7; r++) for (int k = 0; k < 5; k++) if (g[r * 5 + k] == '#') prect(x + k * px, y + r * px, px, px, c);
    }
}

/* ---- audio: a soft looping pad + a blip on click / Space (OpenAL HLE -> SDL audio) ---- */
#define AL(name, n, ...) api("OpenAL32.dll", name, n, ##__VA_ARGS__)
static uint32_t g_blip_src;
static void audio_init(void) {
    uint32_t d = AL("alcOpenDevice", 1, 0u); if (!d) return;
    uint32_t ctx = AL("alcCreateContext", 2, d, 0u); AL("alcMakeContextCurrent", 1, ctx);
    int rate = 44100;
    uint32_t ids = gbuf(8); AL("alGenBuffers", 2, 2u, ids); uint32_t pad = rd32(ids), blip = rd32(ids + 4);
    int n = rate * 4; uint32_t pcm = gbuf((uint32_t)n * 4);   /* 4 s stereo chord, seamless loop (integer Hz) */
    for (int i = 0; i < n; i++) {
        float t = (float)i / rate, e = 0.55f + 0.45f * sinf(6.2831853f * 0.25f * t);
        float l = sinf(6.2831853f * 110 * t) + 0.6f * sinf(6.2831853f * 165 * t) + 0.35f * sinf(6.2831853f * 220 * t);
        float r = sinf(6.2831853f * 110 * t) + 0.6f * sinf(6.2831853f * 165 * t + 1) + 0.35f * sinf(6.2831853f * 277 * t);
        wr16(pcm + 4 * (uint32_t)i, (uint16_t)(int16_t)(2600 * e * l)); wr16(pcm + 4 * (uint32_t)i + 2, (uint16_t)(int16_t)(2600 * e * r));
    }
    AL("alBufferData", 5, pad, 0x1103u /* STEREO16 */, pcm, (uint32_t)n * 4, (uint32_t)rate);
    int bn = rate / 8; uint32_t bp = gbuf((uint32_t)bn * 2);
    for (int i = 0; i < bn; i++) { float t = (float)i / rate; wr16(bp + 2 * (uint32_t)i, (uint16_t)(int16_t)(9000 * expf(-t * 30) * sinf(6.2831853f * (880 + 1200 * t) * t))); }
    AL("alBufferData", 5, blip, 0x1101u /* MONO16 */, bp, (uint32_t)bn * 2, (uint32_t)rate);
    uint32_t s = gbuf(8); AL("alGenSources", 2, 2u, s);
    uint32_t ps = rd32(s); g_blip_src = rd32(s + 4);
    AL("alSourcei", 3, ps, 0x1009u, pad); AL("alSourcei", 3, ps, 0x1007u, 1u); AL("alSourcePlay", 1, ps);
    AL("alSourcei", 3, g_blip_src, 0x1009u, blip);
    fprintf(stderr, "[demo] OpenAL: %d Hz mixer, looping pad started\n", hle_al_rate());
}
static void blip(void) { if (g_blip_src) { AL("alSourceStop", 1, g_blip_src); AL("alSourcePlay", 1, g_blip_src); } }

__attribute__((visibility("default"))) void me1_exe_on_signal(int sig) {
    fprintf(stderr, "\n[demo] signal %d: stopping\n", sig); hle_report(); fflush(stdout); fflush(stderr); _exit(128 + sig);
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int key(int vk) { return (api("user32.dll", "GetAsyncKeyState", 1, (uint32_t)vk) & 0x8000u) != 0; }
static int tapped(int vk) { return (api("user32.dll", "GetAsyncKeyState", 1, (uint32_t)vk) & 0x8001u) != 0; }   /* down now or since the last poll */

__attribute__((visibility("default"))) int me1_exe_main(int argc, char** argv) {
    if (argc < 3) return 2;
    rt_init();
    hle_fs_init(argv[1], argv[2], NULL);
    out = gbuf(64); vb = gbuf(24u * VMAX);
    unsigned sw = 1280, sh = 720;
    uint32_t d3d = api("d3d9.dll", "Direct3DCreate9", 1, 32u);
    uint32_t pp = gbuf(56);
    const unsigned W = 1280, H = 720;   /* back buffer (letterboxed into the window by the presenter) */
    wr32(pp, W); wr32(pp + 4, H); wr32(pp + 8, 22); wr32(pp + 12, 1); wr32(pp + 24, 1); wr32(pp + 28, 0x10000); wr32(pp + 32, 1);
    wr32(pp + 36, 1); wr32(pp + 40, 75);
    if (!d3d || com(d3d, 16, 6, 0u, 1u, 0x10000u, 0x40u, pp, out) || !rd32(out)) { fprintf(stderr, "[demo] CreateDevice failed\n"); return 1; }
    dev = rd32(out);
    fprintf(stderr, "[demo] D3D9 device %ux%u, renderer %s\n", W, H, me1gl_renderer());
    { const char* e = getenv("ME1_SCREEN"); if (e) sscanf(e, "%ux%u", &sw, &sh); }
    { const char* e = getenv("ME1_DEMO_LOAD"); double s = e ? atof(e) : 0;
      if (s > 0) { fprintf(stderr, "[demo] simulated loading: %.1f s\n", s); struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&ts, NULL); } }

    static const uint32_t vs_t[] = { 0xFFFE0300u, DCL(U_POS, 0, R_IN, 0), DCL(U_TC, 0, R_IN, 1), DCL(U_COLOR, 0, R_IN, 2),
        DCL(U_POS, 0, R_OUT, 0), DCL(U_TC, 0, R_OUT, 1), DCL(U_COLOR, 0, R_OUT, 2),
        MOV(DST(R_OUT, 0), SRC(R_IN, 0)), MOV(DST(R_OUT, 1), SRC(R_IN, 1)), MOV(DST(R_OUT, 2), SRC(R_IN, 2)), END };
    static const uint32_t ps_tex[] = { 0xFFFF0300u, DCL(U_TC, 0, R_IN, 0), DCL(U_COLOR, 0, R_IN, 1), DCL2D(0),
        TEXLD(DST(R_TEMP, 0), SRC(R_IN, 0), SRC(R_SAMP, 0)), MUL(DST(R_COLOROUT, 0), SRC(R_TEMP, 0), SRC(R_IN, 1)), END };
    static const uint32_t ps_col[] = { 0xFFFF0300u, DCL(U_COLOR, 0, R_IN, 0), MOV(DST(R_COLOROUT, 0), SRC(R_IN, 0)), END };
    uint32_t vs = shader(91, vs_t, sizeof vs_t / 4), pst = shader(106, ps_tex, sizeof ps_tex / 4), psc = shader(106, ps_col, sizeof ps_col / 4);
    uint32_t de = gbuf(32);
    static const uint8_t decl[32] = { 0,0, 0,0, 2,0,0,0,   0,0, 12,0, 1,0,5,0,   0,0, 20,0, 4,0,10,0,   0xFF,0, 0,0, 17,0,0,0 };
    memcpy(M + de, decl, 32);
    com(dev, 86, 2, de, out); uint32_t vd = rd32(out);
    /* planet texture: 256x256 A8R8G8B8, bands + noise, mip-less, linear filter, wrap */
    com(dev, 23, 8, 256u, 256u, 1u, 0u, 21u, 1u /* MANAGED */, out, 0u); uint32_t tex = rd32(out);
    { uint32_t lr = gbuf(8); com(tex, 19, 4, 0u, lr, 0u, 0u); uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
      uint32_t seed = 12345;
      for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) {
          seed = seed * 1103515245u + 12345u; int nz = (int)(seed >> 27);
          float b = 0.5f + 0.5f * sinf(y * 0.11f + 2.0f * sinf(x * 0.05f)) * cosf(y * 0.023f);
          int r = (int)(40 + 120 * b) + nz, g = (int)(70 + 90 * b) + nz, bl = (int)(140 + 100 * b) + nz;
          if (r > 255) r = 255; if (g > 255) g = 255; if (bl > 255) bl = 255;
          wr32(bits + pitch * (uint32_t)y + 4u * (uint32_t)x, 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl);
      }
      com(tex, 20, 1, 0u); }
    com(dev, 87, 1, vd); com(dev, 92, 1, vs);
    com(dev, 57, 2, 22u, 1u);   /* CULL_NONE */
    com(dev, 57, 2, 7u, 0u);    /* ZENABLE off */
    com(dev, 57, 2, 19u, 5u); com(dev, 57, 2, 20u, 6u);   /* SRCALPHA / INVSRCALPHA */
    com(dev, 69, 3, 0u, 5u, 2u); com(dev, 69, 3, 0u, 6u, 2u);   /* linear */

    audio_init();
    /* stars */
    enum { NS = 260 }; static float sx[NS], sy[NS], sz[NS];
    { uint32_t seed = 777; for (int i = 0; i < NS; i++) { seed = seed * 1664525u + 1013904223u; sx[i] = (seed >> 8) / 16777216.0f * 2 - 1; seed = seed * 1664525u + 1013904223u; sy[i] = (seed >> 8) / 16777216.0f * 2 - 1; seed = seed * 1664525u + 1013904223u; sz[i] = 0.2f + 0.8f * (seed >> 8) / 16777216.0f; } }
    const char* lim = getenv("ME1_DEMO_SECONDS"); double maxs = lim ? atof(lim) : 0;
    const char* shot = getenv("ME1_DEMO_SHOT"); int shot_every = shot ? atoi(shot) : 0;
    double t0 = now_s(), tl = t0; float shipx = 0.55f, shipy = -0.45f; int clicks = 0, wasl = 0, wassp = 0; uint64_t frame = 0;
    uint32_t pt = gbuf(8), xs = gbuf(16), vib = gbuf(4);
    for (;;) {
        double t = now_s(), dt = t - tl; tl = t; float T = (float)(t - t0);
        if (maxs > 0 && t - t0 > maxs) break;
        if (tapped(0x1B)) { fprintf(stderr, "[demo] ESC: quit\n"); break; }
        /* input exactly as the game reads it */
        api("user32.dll", "GetCursorPos", 1, pt);
        float cx = (float)(int32_t)rd32(pt) / sw * 2 - 1, cy = 1 - (float)(int32_t)rd32(pt + 4) / sh * 2;
        unsigned lbs = api("user32.dll", "GetAsyncKeyState", 1, 1u), sps = api("user32.dll", "GetAsyncKeyState", 1, 0x20u);
        int lb = (lbs & 0x8000u) != 0, rb = key(0x02), sp = (sps & 0x8000u) != 0;
        if (((lbs & 1) && !wasl) || ((sps & 1) && !wassp)) { clicks++; blip(); }   /* a new press (bit 0: since the last poll) */
        wasl = lb; wassp = sp;
        float mvx = (key('D') || key(0x27) ? 1.f : 0.f) - (key('A') || key(0x25) ? 1.f : 0.f);
        float mvy = (key('W') || key(0x26) ? 1.f : 0.f) - (key('S') || key(0x28) ? 1.f : 0.f);
        int pad = api("xinput1_3.dll", "XInputGetState", 2, 0u, xs) == 0; unsigned pbtn = 0;
        if (pad) {
            pbtn = rd16(xs + 4);
            float lx = (int16_t)rd16(xs + 8) / 32767.0f, ly = (int16_t)rd16(xs + 10) / 32767.0f;
            if (fabsf(lx) > 0.2f) mvx += lx; if (fabsf(ly) > 0.2f) mvy += ly;
            wr16(vib, rd8(xs + 6) > 30 ? 40000 : 0); wr16(vib + 2, rd8(xs + 7) > 30 ? 40000 : 0);   /* triggers -> rumble */
            api("xinput1_3.dll", "XInputSetState", 2, 0u, vib);
        }
        shipx += mvx * (float)dt * 0.8f; shipy += mvy * (float)dt * 0.8f;
        if (shipx < -0.95f) shipx = -0.95f; if (shipx > 0.95f) shipx = 0.95f; if (shipy < -0.9f) shipy = -0.9f; if (shipy > 0.9f) shipy = 0.9f;

        com(dev, 41, 0);   /* BeginScene */
        uint32_t bg = 0xFF000000u | (uint32_t)(6 + 4 * sinf(T * 0.3f)) << 16 | (uint32_t)(8 + 4 * sinf(T * 0.2f)) << 8 | (uint32_t)(22 + 8 * sinf(T * 0.25f));
        com(dev, 43, 6, 0u, 0u, 3u, bg, fb(1.0f), 0u);
        com(dev, 107, 1, psc); com(dev, 57, 2, 27u, 1u);
        /* nebula glow + stars (parallax, twinkle) */
        disc(-0.35f, 0.15f, 1.3f, 0x50402070u, 0x00100818u, 64, 0, 1);
        disc(0.6f, 0.5f, 0.7f, 0x30205090u, 0x00000000u, 48, 0, 1);
        for (int i = 0; i < NS; i++) {
            float x = sx[i] - fmodf(T * 0.02f * sz[i], 2.0f); if (x < -1) x += 2;
            float tw = 0.6f + 0.4f * sinf(T * 3 * sz[i] + i), s = 0.0025f + 0.004f * sz[i];
            uint32_t a = (uint32_t)(255 * tw * sz[i]);
            rect(x - s / g_aspect, sy[i] + s, x + s / g_aspect, sy[i] - s, a << 24 | 0xE0E8FFu);
        }
        flush();
        /* planet: textured disc, rotating */
        com(dev, 107, 1, pst); com(dev, 65, 2, 0u, tex);
        disc(-0.3f, 0.0f, 0.55f, 0xFFFFFFFFu, 0xFF203048u, 96, T * 0.15f, 1.0f);
        flush();
        com(dev, 65, 2, 0u, 0u); com(dev, 107, 1, psc);
        ring(-0.3f, 0.0f, 0.55f, 0.63f, 0x6080C0FFu, 0x00000000u, 96, 1.0f);            /* atmosphere */
        ring(-0.3f, 0.0f, 0.72f, 0.95f, 0x70C09060u, 0x20806040u, 128, 0.22f);           /* rings */
        /* relay: spinning arcs */
        for (int k = 0; k < 3; k++) {
            float a = T * (0.8f + 0.3f * k) + k * 2.1f, r = 0.16f + 0.05f * k;
            for (int i = 0; i < 24; i++) {
                float a0 = a + i * 0.11f, a1 = a0 + 0.1f;
                float x00 = 0.6f + r * cosf(a0) / g_aspect, y00 = 0.35f + r * sinf(a0), x01 = 0.6f + (r + 0.015f) * cosf(a0) / g_aspect, y01 = 0.35f + (r + 0.015f) * sinf(a0);
                float x10 = 0.6f + r * cosf(a1) / g_aspect, y10 = 0.35f + r * sinf(a1), x11 = 0.6f + (r + 0.015f) * cosf(a1) / g_aspect, y11 = 0.35f + (r + 0.015f) * sinf(a1);
                uint32_t c = 0xD040A0FFu;
                v(x00, y00, 0, 0, c); v(x01, y01, 0, 0, c); v(x10, y10, 0, 0, c); v(x01, y01, 0, 0, c); v(x11, y11, 0, 0, c); v(x10, y10, 0, 0, c);
            }
        }
        disc(0.6f, 0.35f, 0.05f + 0.01f * sinf(T * 4), 0xFFFFFFFFu, 0x0040A0FFu, 32, 0, 1);
        /* ship (WASD / arrows / left stick) */
        { float s = 0.05f; uint32_t c = pad && (pbtn & 0x1000) ? 0xFFFFC040u : 0xFFE0E0E8u;
          v(shipx + s * 1.6f / g_aspect, shipy, 0, 0, c); v(shipx - s / g_aspect, shipy + s * 0.7f, 0, 0, 0xFF8090A0u); v(shipx - s / g_aspect, shipy - s * 0.7f, 0, 0, 0xFF8090A0u);
          disc(shipx - s * 1.1f / g_aspect, shipy, 0.02f + 0.006f * sinf(T * 30), 0xFFFFFFFFu, 0x0040C0FFu, 16, 0, 1); }
        /* HUD */
        prect(24, 22, 560, 118, 0xA0000000u);
        text(40, 36, 4, "MASS EFFECT  LINUX PORT", 0xFFE8EEF8u);
        int64_t pf[4] = { 0 }; me1_perf(pf);
        char line[160];
        snprintf(line, sizeof line, "%s  %lld.%lld FPS", me1gl_renderer(), (long long)(pf[0] / 10), (long long)(pf[0] % 10));
        text(40, 76, 3, line, 0xFF80C8FFu);
        snprintf(line, sizeof line, "MOUSE %d,%d  CLICKS %d  %s%s", (int)rd32(pt), (int)rd32(pt + 4), clicks, pad ? "PAD OK" : "NO PAD", rb ? "  RMB" : "");
        text(40, 104, 3, line, 0xFFB0B8C8u);
        text(24, 690, 2, "WASD / ARROWS: MOVE   CLICK / SPACE: BLIP   ALT+ENTER: FULLSCREEN   CTRL+ALT+M: MOUSE   ESC: QUIT", 0xC0A0A8B8u);
        /* cursor */
        { float px = cx, py = cy, s = 0.025f; uint32_t c = lb ? 0xFFFF5040u : rb ? 0xFF40FF80u : 0xFFFFFFFFu;
          rect(px - s / g_aspect, py + 0.003f, px + s / g_aspect, py - 0.003f, c); rect(px - 0.003f / g_aspect, py + s, px + 0.003f / g_aspect, py - s, c); }
        flush();
        com(dev, 57, 2, 27u, 0u);
        com(dev, 42, 0);   /* EndScene */
        com(dev, 17, 4, 0u, 0u, 0u, 0u);   /* Present */
        frame++;
        if (shot_every && frame % (uint64_t)shot_every == 0) fprintf(stderr, "[demo] frame %llu, t %.2f, cursor %d,%d, clicks %d, pad %d\n", (unsigned long long)frame, t - t0, (int)rd32(pt), (int)rd32(pt + 4), clicks, pad);
        usleep(1000);
    }
    fprintf(stderr, "[demo] %llu frames in %.1f s\n", (unsigned long long)frame, now_s() - t0);
    hle_report();
    return 0;
}
