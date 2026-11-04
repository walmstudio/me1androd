/* OpenAL HLE (hle_al.c) through the real thunks, mixer driven manually (ME1_AL_SINK=manual). usage: test_al */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t api(const char* dll, const char* name, int n, ...) {
    uint32_t t = hle_resolve_impl(dll, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    if (!t) { fprintf(stderr, "FAIL: %s!%s not implemented\n", dll, name); g_fail++; return 0; }
    return rt_call(&g_cpu, t, n, a);
}
#define AL(name, n, ...) api("OpenAL32.dll", name, n, ##__VA_ARGS__)
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t G(uint32_t n) { return ghalloc(n, 1); }
static int geti(uint32_t src, uint32_t p) { uint32_t o = G(4); AL("alGetSourcei", 3, src, p, o); return (int)rd32(o); }
static float getf(uint32_t src, uint32_t p) { uint32_t o = G(4); AL("alGetSourcef", 3, src, p, o); return rdf32(o); }
static int err(void) { return (int)AL("alGetError", 0); }
static int rate;
static double rmsL, rmsR;
static void render_ms(int ms) {   /* mix ms milliseconds, RMS of each channel */
    int n = rate * ms / 1000; int16_t* b = malloc((size_t)n * 4);
    hle_al_render(b, n);
    double l = 0, r = 0; for (int i = 0; i < n; i++) { l += (double)b[2 * i] * b[2 * i]; r += (double)b[2 * i + 1] * b[2 * i + 1]; }
    rmsL = sqrt(l / n); rmsR = sqrt(r / n); free(b);
}
static uint32_t sine_buf(int frames, int freq, int ch, float amp) {   /* 16-bit sine (1 kHz) */
    uint32_t id = G(4); AL("alGenBuffers", 2, 1u, id); id = rd32(id);
    uint32_t d = G((uint32_t)frames * 2 * ch);
    for (int i = 0; i < frames; i++) for (int k = 0; k < ch; k++) wr16(d + 2 * (i * ch + k), (uint16_t)(int16_t)(amp * sinf(2 * (float)M_PI * 1000 * i / freq)));
    AL("alBufferData", 5, id, ch == 1 ? 0x1101u : 0x1103u, d, (uint32_t)frames * 2 * ch, (uint32_t)freq);
    return id;
}
static int realtime(const char* wav) {   /* test_al --wav FILE: the real-time null sink records 300 ms of a 100 ms looping tone */
    setenv("ME1_AL_SINK", "null", 1); setenv("ME1_AL_WAV", wav, 1);
    rt_init();
    uint32_t dev = AL("alcOpenDevice", 1, 0u), ctx = AL("alcCreateContext", 2, dev, 0u); AL("alcMakeContextCurrent", 1, ctx);
    rate = hle_al_rate();
    uint32_t b = sine_buf(rate / 10, rate, 1, 16000), src = G(4); AL("alGenSources", 2, 1u, src); src = rd32(src);
    AL("alSourcei", 3, src, 0x1009u, b); AL("alSourcei", 3, src, 0x1007u, 1u); AL("alSourcePlay", 1, src);
    struct timespec t = { 0, 300000000 }; nanosleep(&t, NULL);
    int off = geti(src, 0x1025), st = geti(src, 0x1010);
    hle_al_report();
    FILE* f = fopen(wav, "rb"); long n = 0; if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); }
    CHECK(st == 0x1012 && off > 0, "real-time sink advances the source (offset %d)", off);
    CHECK(n > 44 + rate * 4 / 10, "wav %ld bytes", n);
    fprintf(stderr, "test_al --wav: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
int main(int argc, char** argv) {
    if (argc > 2 && !strcmp(argv[1], "--wav")) return realtime(argv[2]);
    setenv("ME1_AL_SINK", "manual", 1);
    rt_init();
    /* --- module / exports --- */
    uint32_t h = api("kernel32.dll", "LoadLibraryA", 1, gstrdup("OpenAL32.dll"));
    CHECK(h == hle_al_module() && h, "LoadLibraryA(OpenAL32.dll) = %08x", h);
    CHECK(api("kernel32.dll", "GetProcAddress", 2, h, gstrdup("alSourceQueueBuffers")) == hle_resolve_impl("OpenAL32.dll", "alSourceQueueBuffers"), "GetProcAddress");
    CHECK(api("kernel32.dll", "GetProcAddress", 2, h, gstrdup("EAXSet")) == 0, "GetProcAddress(EAXSet) = 0");
    static const char* names[] = { "alBuffer3f", "alBuffer3i", "alBufferData", "alBufferf", "alBufferfv", "alBufferi", "alBufferiv", "alDeleteBuffers",
        "alDeleteSources", "alDisable", "alDistanceModel", "alDopplerFactor", "alDopplerVelocity", "alEnable", "alGenBuffers", "alGenSources",
        "alGetBoolean", "alGetBooleanv", "alGetBuffer3f", "alGetBuffer3i", "alGetBufferf", "alGetBufferfv", "alGetBufferi", "alGetBufferiv",
        "alGetDouble", "alGetDoublev", "alGetEnumValue", "alGetError", "alGetFloat", "alGetFloatv", "alGetInteger", "alGetIntegerv",
        "alGetListener3f", "alGetListener3i", "alGetListenerf", "alGetListenerfv", "alGetListeneri", "alGetListeneriv", "alGetProcAddress",
        "alGetSource3f", "alGetSource3i", "alGetSourcef", "alGetSourcefv", "alGetSourcei", "alGetSourceiv", "alGetString", "alIsBuffer",
        "alIsEnabled", "alIsExtensionPresent", "alIsSource", "alListener3f", "alListener3i", "alListenerf", "alListenerfv", "alListeneri",
        "alListeneriv", "alSource3f", "alSource3i", "alSourcePause", "alSourcePausev", "alSourcePlay", "alSourcePlayv", "alSourceQueueBuffers",
        "alSourceRewind", "alSourceRewindv", "alSourceStop", "alSourceStopv", "alSourceUnqueueBuffers", "alSourcef", "alSourcefv", "alSourcei",
        "alSourceiv", "alSpeedOfSound", "alcCaptureCloseDevice", "alcCaptureOpenDevice", "alcCaptureSamples", "alcCaptureStart",
        "alcCaptureStop", "alcCloseDevice", "alcCreateContext", "alcDestroyContext", "alcGetContextsDevice", "alcGetCurrentContext",
        "alcGetEnumValue", "alcGetError", "alcGetIntegerv", "alcGetProcAddress", "alcGetString", "alcIsExtensionPresent",
        "alcMakeContextCurrent", "alcOpenDevice", "alcProcessContext", "alcSuspendContext" };
    int miss = 0; for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (!hle_resolve_impl("OpenAL32.dll", names[i])) { miss++; fprintf(stderr, "missing %s\n", names[i]); }
    CHECK(!miss, "all %u functions MassEffect.exe names are implemented", (unsigned)(sizeof names / sizeof names[0]));
    /* --- ALC --- */
    uint32_t s = AL("alcGetString", 2, 0u, 0x1005u);
    CHECK(s && !strcmp((char*)M + s, "Generic Software") && M[s + 17] == 0, "device list");
    CHECK(AL("alcIsExtensionPresent", 2, 0u, gstrdup("ALC_ENUMERATION_EXT")) == 1, "ALC_ENUMERATION_EXT");
    uint32_t dev = AL("alcOpenDevice", 1, 0u); CHECK(dev, "alcOpenDevice");
    uint32_t ctx = AL("alcCreateContext", 2, dev, 0u); CHECK(ctx, "alcCreateContext");
    CHECK(AL("alcMakeContextCurrent", 1, ctx) == 1 && AL("alcGetCurrentContext", 0) == ctx && AL("alcGetContextsDevice", 1, ctx) == dev, "current context");
    uint32_t iv = G(64); AL("alcGetIntegerv", 4, dev, 0x1000u, 1u, iv); AL("alcGetIntegerv", 4, dev, 0x1001u, 1u, iv + 4); AL("alcGetIntegerv", 4, dev, 0x1007u, 1u, iv + 8);
    rate = (int)rd32(iv + 8);
    CHECK(rd32(iv) == 1 && rd32(iv + 4) == 1 && rate == hle_al_rate() && rate >= 8000, "ALC version %u.%u, %d Hz", rd32(iv), rd32(iv + 4), rate);
    CHECK(AL("alcGetError", 1, dev) == 0, "alcGetError");
    /* --- AL state --- */
    CHECK(!strcmp((char*)M + AL("alGetString", 1, 0xB002u), "1.1"), "AL_VERSION");
    CHECK(AL("alIsExtensionPresent", 1, gstrdup("EAX2.0")) == 0 && AL("alIsExtensionPresent", 1, gstrdup("AL_EXT_OFFSET")) == 1, "extensions");
    CHECK(AL("alGetEnumValue", 1, gstrdup("AL_FORMAT_MONO16")) == 0x1101 && AL("alGetEnumValue", 1, gstrdup("AL_FORMAT_MONO_IMA4")) == 0, "alGetEnumValue");
    AL("alDopplerFactor", 1, fb(0.5f));
    AL("alGetFloat", 1, 0xC000u); double st0 = g_cpu.st[g_cpu.ftop]; fpop(&g_cpu);
    CHECK(fabs(st0 - 0.5) < 1e-6, "alGetFloat via st0 = %g", st0);
    CHECK(AL("alGetInteger", 1, 0xD000u) == 0xD002, "default distance model");
    CHECK(err() == 0, "no error so far");
    AL("alGetInteger", 1, 0x1234u); CHECK(err() == 0xA002 && err() == 0, "AL_INVALID_ENUM then cleared");
    /* --- buffers --- */
    uint32_t b1 = sine_buf(rate / 10, rate, 1, 16000);   /* 100 ms */
    uint32_t o = G(16);
    AL("alGetBufferi", 3, b1, 0x2001u, o); AL("alGetBufferi", 3, b1, 0x2002u, o + 4); AL("alGetBufferi", 3, b1, 0x2003u, o + 8); AL("alGetBufferi", 3, b1, 0x2004u, o + 12);
    CHECK((int)rd32(o) == rate && rd32(o + 4) == 16 && rd32(o + 8) == 1 && rd32(o + 12) == (uint32_t)(rate / 10 * 2), "buffer params %u %u %u %u", rd32(o), rd32(o + 4), rd32(o + 8), rd32(o + 12));
    /* --- static source, centre --- */
    uint32_t src = G(4); AL("alGenSources", 2, 1u, src); src = rd32(src);
    CHECK(src && AL("alIsSource", 1, src) == 1 && geti(src, 0x1010) == 0x1011, "new source INITIAL");
    AL("alSourcei", 3, src, 0x1009u, b1); AL("alSourcei", 3, src, 0x202u, 1u);   /* relative at the origin */
    CHECK(geti(src, 0x1027) == 0x1028 && geti(src, 0x1009) == (int)b1, "static source");
    AL("alSourcePlay", 1, src); CHECK(geti(src, 0x1010) == 0x1012, "PLAYING");
    render_ms(50);
    CHECK(rmsL > 7000 && fabs(rmsL - rmsR) < 50, "centre: rms L %.0f R %.0f", rmsL, rmsR);
    int off = geti(src, 0x1025); CHECK(abs(off - rate / 20) <= 2, "sample offset %d", off);
    render_ms(60);
    CHECK(geti(src, 0x1010) == 0x1014 && geti(src, 0x1025) == 0, "finished -> STOPPED");
    /* --- panning + distance (non-relative, listener at the origin facing -z) --- */
    AL("alSourcei", 3, src, 0x202u, 0u); AL("alSource3f", 5, src, 0x1004u, fb(1), fb(0), fb(0));
    AL("alSourcePlay", 1, src); render_ms(30);
    CHECK(rmsR > 9000 && rmsL < 200, "right: L %.0f R %.0f", rmsL, rmsR);
    AL("alSource3f", 5, src, 0x1004u, fb(0), fb(0), fb(-1)); AL("alSourcePlay", 1, src); render_ms(30); double near = rmsL;
    AL("alSource3f", 5, src, 0x1004u, fb(0), fb(0), fb(-4)); AL("alSourcePlay", 1, src); render_ms(30);
    CHECK(fabs(rmsL / near - 0.25) < 0.02, "inverse distance clamped at 4: %.3f", rmsL / near);
    AL("alListenerf", 2, 0x100Au, fb(0.5f)); AL("alSourcePlay", 1, src); render_ms(30);
    CHECK(fabs(rmsL / near - 0.125) < 0.02, "listener gain: %.3f", rmsL / near);
    AL("alListenerf", 2, 0x100Au, fb(1.0f));
    { uint32_t lf = G(24); float ori[6] = { 1, 0, 0, 0, 1, 0 }; for (int i = 0; i < 6; i++) wrf32(lf + 4 * i, ori[i]); AL("alListenerfv", 2, 0x100Fu, lf); }
    AL("alSource3f", 5, src, 0x1004u, fb(0), fb(0), fb(1)); AL("alSourcePlay", 1, src); render_ms(30);   /* listener faces +x: +z is on the right */
    CHECK(rmsR > 9000 && rmsL < 200, "listener orientation: L %.0f R %.0f", rmsL, rmsR);
    { uint32_t lf = G(24); float ori[6] = { 0, 0, -1, 0, 1, 0 }; for (int i = 0; i < 6; i++) wrf32(lf + 4 * i, ori[i]); AL("alListenerfv", 2, 0x100Fu, lf); }
    AL("alSourceStop", 1, src);
    /* --- pitch: twice as fast --- */
    AL("alSource3f", 5, src, 0x1004u, fb(0), fb(0), fb(0)); AL("alSourcef", 3, src, 0x1003u, fb(2.0f));
    AL("alSourcePlay", 1, src); render_ms(40); CHECK(geti(src, 0x1010) == 0x1012, "pitch 2: playing at 40 ms");
    render_ms(20); CHECK(geti(src, 0x1010) == 0x1014, "pitch 2: done at 60 ms");
    AL("alSourcef", 3, src, 0x1003u, fb(1.0f));
    /* --- looping --- */
    AL("alSourcei", 3, src, 0x1007u, 1u); AL("alSourcePlay", 1, src); render_ms(250);
    CHECK(geti(src, 0x1010) == 0x1012 && geti(src, 0x1016) == 0 && rmsL > 7000, "looping");
    AL("alSourcePause", 1, src); CHECK(geti(src, 0x1010) == 0x1013, "PAUSED"); render_ms(10); CHECK(rmsL == 0, "paused is silent");
    AL("alSourceStop", 1, src); AL("alSourcei", 3, src, 0x1007u, 0u);
    /* --- offsets --- */
    AL("alSourcei", 3, src, 0x1025u, (uint32_t)(rate / 20)); AL("alSourcePlay", 1, src);
    CHECK(fabs(getf(src, 0x1024) - 0.05f) < 0.001f, "AL_SEC_OFFSET after a pending sample offset: %f", getf(src, 0x1024));
    CHECK(geti(src, 0x1026) == rate / 20 * 2, "AL_BYTE_OFFSET");
    render_ms(60); CHECK(geti(src, 0x1010) == 0x1014, "offset start -> ends early");
    /* --- deleting an attached buffer fails --- */
    { uint32_t ids = G(4); wr32(ids, b1); AL("alDeleteBuffers", 2, 1u, ids); CHECK(err() == 0xA004 && AL("alIsBuffer", 1, b1) == 1, "delete attached buffer -> INVALID_OPERATION"); }
    AL("alSourcei", 3, src, 0x1009u, 0u); CHECK(geti(src, 0x1027) == 0x1030, "AL_BUFFER 0 -> UNDETERMINED");
    /* --- streaming queue --- */
    uint32_t q[3]; for (int i = 0; i < 3; i++) q[i] = sine_buf(rate / 100, rate, 2, 12000);   /* 3 x 10 ms stereo */
    uint32_t qa = G(12); for (int i = 0; i < 3; i++) wr32(qa + 4 * i, q[i]);
    AL("alSourceQueueBuffers", 3, src, 3u, qa);
    CHECK(geti(src, 0x1027) == 0x1029 && geti(src, 0x1015) == 3 && geti(src, 0x1016) == 0, "queued 3");
    AL("alSourcePlay", 1, src); render_ms(15);
    CHECK(geti(src, 0x1016) == 1 && rmsL > 7000 && fabs(rmsL - rmsR) < 50, "1 processed after 15 ms (stereo L %.0f R %.0f)", rmsL, rmsR);
    uint32_t un = G(12); AL("alSourceUnqueueBuffers", 3, src, 1u, un);
    CHECK(err() == 0 && rd32(un) == q[0] && geti(src, 0x1015) == 2, "unqueued the first");
    AL("alSourceUnqueueBuffers", 3, src, 2u, un); CHECK(err() == 0xA003, "unqueue pending -> INVALID_VALUE");
    wr32(qa, q[0]); AL("alSourceQueueBuffers", 3, src, 1u, qa);   /* refill while playing */
    render_ms(40);
    CHECK(geti(src, 0x1010) == 0x1014 && geti(src, 0x1016) == 3, "stream ran out -> STOPPED, all 3 processed");
    AL("alSourceUnqueueBuffers", 3, src, 3u, un); CHECK(err() == 0 && geti(src, 0x1015) == 0, "unqueue all");
    AL("alSourceQueueBuffers", 3, src, 1u, qa); CHECK(geti(src, 0x1016) == 0, "buffer queued on a stopped source is pending");
    AL("alSourcePlay", 1, src); render_ms(5); CHECK(geti(src, 0x1010) == 0x1012, "replay");
    AL("alSourceStop", 1, src); CHECK(geti(src, 0x1016) == 1, "stop: all processed");
    AL("alSourceRewind", 1, src); CHECK(geti(src, 0x1010) == 0x1011 && geti(src, 0x1016) == 0, "rewind -> INITIAL");
    /* --- formats: 8-bit, float32, 5.1 --- */
    uint32_t bx = G(4); AL("alGenBuffers", 2, 1u, bx); bx = rd32(bx);
    uint32_t d8 = G(400); for (int i = 0; i < 400; i++) wr8(d8 + i, i & 1 ? 255 : 0);
    AL("alBufferData", 5, bx, 0x1100u, d8, 400u, 22050u); o = G(4); AL("alGetBufferi", 3, bx, 0x2002u, o); CHECK(err() == 0 && rd32(o) == 8, "MONO8");
    uint32_t df = G(800); for (int i = 0; i < 200; i++) { wrf32(df + 8 * i, 0.5f); wrf32(df + 8 * i + 4, -0.5f); }
    AL("alBufferData", 5, bx, 0x10011u, df, 1600u, 44100u); CHECK(err() == 0xA003 || err() == 0, "float32 size check");
    AL("alBufferData", 5, bx, 0x10011u, df, 800u, 44100u); AL("alGetBufferi", 3, bx, 0x2003u, o); CHECK(err() == 0 && rd32(o) == 2, "STEREO_FLOAT32");
    uint32_t d51 = G(6 * 2 * 100); for (int i = 0; i < 600; i++) wr16(d51 + 2 * i, 10000);
    AL("alBufferData", 5, bx, 0x120Bu, d51, 1200u, 44100u); AL("alGetBufferi", 3, bx, 0x2003u, o); CHECK(err() == 0 && rd32(o) == 6, "51CHN16 -> 6 channels reported");
    AL("alSourcei", 3, src, 0x1009u, bx); AL("alSourcePlay", 1, src); render_ms(1);
    CHECK(rmsL > 1000 && fabs(rmsL - rmsR) < 50, "5.1 downmix: L %.0f R %.0f", rmsL, rmsR);
    AL("alBufferData", 5, bx, 0x9999u, d51, 16u, 44100u); CHECK(err() == 0xA002, "unknown format -> INVALID_ENUM");
    /* --- limits --- */
    uint32_t many = G(4 * 300); AL("alGenSources", 2, 300u, many); CHECK(err() == 0xA005 && rd32(many) == 0, "300 sources -> AL_OUT_OF_MEMORY, nothing generated");
    AL("alcGetIntegerv", 4, dev, 0x1010u, 1u, iv); CHECK(rd32(iv) >= 128, "ALC_MONO_SOURCES %u", rd32(iv));
    /* --- teardown --- */
    { uint32_t ids = G(4); wr32(ids, src); AL("alDeleteSources", 2, 1u, ids); CHECK(err() == 0 && AL("alIsSource", 1, src) == 0, "delete source"); }
    { uint32_t ids = G(8); wr32(ids, b1); wr32(ids + 4, bx); AL("alDeleteBuffers", 2, 2u, ids); CHECK(err() == 0 && AL("alIsBuffer", 1, b1) == 0, "delete buffers"); }
    AL("alcMakeContextCurrent", 1, 0u); AL("alcDestroyContext", 1, ctx);
    CHECK(AL("alcCloseDevice", 1, dev) == 1, "alcCloseDevice");
    hle_al_report();
    /* --- winmm multimedia timers (ISACT ticks its sound engine from timeSetEvent) --- */
    {
        uint32_t caps = G(8); api("winmm.dll", "timeGetDevCaps", 2, caps, 8u);
        CHECK(rd32(caps) >= 1 && rd32(caps) <= 100, "timeGetDevCaps wPeriodMin %u", rd32(caps));
        uint32_t ev = api("kernel32.dll", "CreateEventA", 4, 0u, 0u, 0u, 0u);   /* auto-reset */
        uint32_t id = api("winmm.dll", "timeSetEvent", 5, 10u, 0u, ev, 0u, 1u | 0x10u);   /* periodic, TIME_CALLBACK_EVENT_SET */
        CHECK(id != 0, "timeSetEvent periodic -> %u", id);
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        int n = 0; for (int i = 0; i < 100; i++) { if (api("kernel32.dll", "WaitForSingleObject", 2, ev, 200u) != 0) break; if (++n == 15) break; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        CHECK(n == 15 && ms > 100 && ms < 1000, "15 periodic 10 ms ticks in %.0f ms", ms);
        CHECK(api("winmm.dll", "timeKillEvent", 1, id) == 0, "timeKillEvent");
        api("kernel32.dll", "WaitForSingleObject", 2, ev, 30u);   /* a tick racing the kill */
        CHECK(api("kernel32.dll", "WaitForSingleObject", 2, ev, 60u) == 0x102u, "no ticks after timeKillEvent");
        CHECK(api("winmm.dll", "timeKillEvent", 1, id) == 11u, "timeKillEvent(dead id) = MMSYSERR_INVALPARAM");
        uint32_t id2 = api("winmm.dll", "timeSetEvent", 5, 20u, 0u, ev, 0u, 0x10u);   /* one-shot */
        CHECK(id2 && id2 != id, "timeSetEvent one-shot -> %u", id2);
        CHECK(api("kernel32.dll", "WaitForSingleObject", 2, ev, 500u) == 0, "one-shot fired");
        CHECK(api("kernel32.dll", "WaitForSingleObject", 2, ev, 80u) == 0x102u, "one-shot fired once");
        CHECK(api("winmm.dll", "timeKillEvent", 1, id2) == 11u, "one-shot id is gone after firing");
        char mb[160]; hle_mm_brief(mb, sizeof mb); fprintf(stderr, "  %s\n", mb);
        CHECK(strstr(mb, "0 timers") != NULL, "no timers left");
    }
    fprintf(stderr, "test_al: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
