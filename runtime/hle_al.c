/* OpenAL 1.1 (OpenAL32.dll) for MassEffect.exe. ISACT (the game's audio engine) imports 36 alc/al functions statically and
 * UE3 loads OpenAL32.dll itself (LoadLibrary + GetProcAddress of ~90 functions, alGetProcAddress as fallback); without a
 * device the engine took its rarely used "no audio" path. All entry points are cdecl (nargs 0: the caller pops).
 *
 * Software implementation: one device ("Generic Software"), contexts sharing one source/buffer state, up to 256 sources;
 * buffers are stored as int16 mono or stereo (8/16-bit, float32, IMA4 and the AL_EXT_MCFORMATS layouts are converted,
 * multichannel is downmixed); sources: static/streaming queues with processed/pending semantics as in OpenAL Soft,
 * looping, offsets, pitch (linear interpolation), distance models, cones, min/max gain, equal-power panning of mono
 * sources in listener space (stereo sources are not spatialised), gain ramps per mix block. No EAX, no doppler shift.
 * Output: AAudio (Android, libaaudio.so via dlopen, 16-bit stereo at the device rate, data callback mixes directly;
 * the stream is reopened after a disconnect), else a real-time "null" sink thread (10 ms periods).
 * Environment: ME1_AL=0 off (no OpenAL32.dll, alcOpenDevice -> NULL), ME1_AL_GAIN=<master gain, default 1>,
 * ME1_AL_RATE=<Hz> (null sink, default 44100; AAudio: requested rate), ME1_AL_SINK=null|manual (manual: tests call
 * hle_al_render), ME1_AL_WAV=<file> (null sink: record the mix), ME1_AL_IMA4=1 (expose AL_FORMAT_*_IMA4),
 * ME1_AL_LOG=2 (log more calls). */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <float.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>

#define AL_HMOD 0x7C900000u
/* ---- enums (al.h / alc.h / alext.h) ---- */
enum {
    AL_NONE = 0, AL_SOURCE_DISTANCE_MODEL = 0x200, AL_SOURCE_RELATIVE = 0x202, AL_CONE_INNER_ANGLE = 0x1001, AL_CONE_OUTER_ANGLE = 0x1002,
    AL_PITCH = 0x1003, AL_POSITION = 0x1004, AL_DIRECTION = 0x1005, AL_VELOCITY = 0x1006, AL_LOOPING = 0x1007, AL_BUFFER = 0x1009,
    AL_GAIN = 0x100A, AL_MIN_GAIN = 0x100D, AL_MAX_GAIN = 0x100E, AL_ORIENTATION = 0x100F, AL_SOURCE_STATE = 0x1010,
    AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013, AL_STOPPED = 0x1014, AL_BUFFERS_QUEUED = 0x1015,
    AL_BUFFERS_PROCESSED = 0x1016, AL_REFERENCE_DISTANCE = 0x1020, AL_ROLLOFF_FACTOR = 0x1021, AL_CONE_OUTER_GAIN = 0x1022,
    AL_MAX_DISTANCE = 0x1023, AL_SEC_OFFSET = 0x1024, AL_SAMPLE_OFFSET = 0x1025, AL_BYTE_OFFSET = 0x1026, AL_SOURCE_TYPE = 0x1027,
    AL_STATIC = 0x1028, AL_STREAMING = 0x1029, AL_UNDETERMINED = 0x1030,
    AL_FREQUENCY = 0x2001, AL_BITS = 0x2002, AL_CHANNELS = 0x2003, AL_SIZE = 0x2004,
    AL_INVALID_NAME = 0xA001, AL_INVALID_ENUM = 0xA002, AL_INVALID_VALUE = 0xA003, AL_INVALID_OPERATION = 0xA004, AL_OUT_OF_MEMORY = 0xA005,
    AL_VENDOR = 0xB001, AL_VERSION = 0xB002, AL_RENDERER = 0xB003, AL_EXTENSIONS = 0xB004,
    AL_DOPPLER_FACTOR = 0xC000, AL_DOPPLER_VELOCITY = 0xC001, AL_SPEED_OF_SOUND = 0xC003, AL_DISTANCE_MODEL = 0xD000,
    AL_INVERSE_DISTANCE = 0xD001, AL_INVERSE_DISTANCE_CLAMPED = 0xD002, AL_LINEAR_DISTANCE = 0xD003, AL_LINEAR_DISTANCE_CLAMPED = 0xD004,
    AL_EXPONENT_DISTANCE = 0xD005, AL_EXPONENT_DISTANCE_CLAMPED = 0xD006,
    ALC_MAJOR_VERSION = 0x1000, ALC_MINOR_VERSION = 0x1001, ALC_ATTRIBUTES_SIZE = 0x1002, ALC_ALL_ATTRIBUTES = 0x1003,
    ALC_DEFAULT_DEVICE_SPECIFIER = 0x1004, ALC_DEVICE_SPECIFIER = 0x1005, ALC_EXTENSIONS = 0x1006, ALC_FREQUENCY = 0x1007,
    ALC_REFRESH = 0x1008, ALC_SYNC = 0x1009, ALC_MONO_SOURCES = 0x1010, ALC_STEREO_SOURCES = 0x1011,
    ALC_DEFAULT_ALL_DEVICES_SPECIFIER = 0x1012, ALC_ALL_DEVICES_SPECIFIER = 0x1013,
    ALC_CAPTURE_DEVICE_SPECIFIER = 0x310, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER = 0x311, ALC_CAPTURE_SAMPLES = 0x312,
    ALC_INVALID_DEVICE = 0xA001, ALC_INVALID_CONTEXT = 0xA002, ALC_INVALID_ENUM = 0xA003, ALC_INVALID_VALUE = 0xA004,
};
static const struct { const char* n; int v; } g_enums[] = {
    { "AL_NONE", 0 }, { "AL_FALSE", 0 }, { "AL_TRUE", 1 }, { "AL_SOURCE_RELATIVE", 0x202 }, { "AL_SOURCE_DISTANCE_MODEL", 0x200 },
    { "AL_CONE_INNER_ANGLE", 0x1001 }, { "AL_CONE_OUTER_ANGLE", 0x1002 }, { "AL_PITCH", 0x1003 }, { "AL_POSITION", 0x1004 },
    { "AL_DIRECTION", 0x1005 }, { "AL_VELOCITY", 0x1006 }, { "AL_LOOPING", 0x1007 }, { "AL_BUFFER", 0x1009 }, { "AL_GAIN", 0x100A },
    { "AL_MIN_GAIN", 0x100D }, { "AL_MAX_GAIN", 0x100E }, { "AL_ORIENTATION", 0x100F }, { "AL_SOURCE_STATE", 0x1010 },
    { "AL_INITIAL", 0x1011 }, { "AL_PLAYING", 0x1012 }, { "AL_PAUSED", 0x1013 }, { "AL_STOPPED", 0x1014 },
    { "AL_BUFFERS_QUEUED", 0x1015 }, { "AL_BUFFERS_PROCESSED", 0x1016 }, { "AL_REFERENCE_DISTANCE", 0x1020 },
    { "AL_ROLLOFF_FACTOR", 0x1021 }, { "AL_CONE_OUTER_GAIN", 0x1022 }, { "AL_MAX_DISTANCE", 0x1023 }, { "AL_SEC_OFFSET", 0x1024 },
    { "AL_SAMPLE_OFFSET", 0x1025 }, { "AL_BYTE_OFFSET", 0x1026 }, { "AL_SOURCE_TYPE", 0x1027 }, { "AL_STATIC", 0x1028 },
    { "AL_STREAMING", 0x1029 }, { "AL_UNDETERMINED", 0x1030 }, { "AL_FORMAT_MONO8", 0x1100 }, { "AL_FORMAT_MONO16", 0x1101 },
    { "AL_FORMAT_STEREO8", 0x1102 }, { "AL_FORMAT_STEREO16", 0x1103 }, { "AL_FREQUENCY", 0x2001 }, { "AL_BITS", 0x2002 },
    { "AL_CHANNELS", 0x2003 }, { "AL_SIZE", 0x2004 }, { "AL_UNUSED", 0x2010 }, { "AL_PENDING", 0x2011 }, { "AL_PROCESSED", 0x2012 },
    { "AL_NO_ERROR", 0 }, { "AL_INVALID_NAME", 0xA001 }, { "AL_INVALID_ENUM", 0xA002 }, { "AL_INVALID_VALUE", 0xA003 },
    { "AL_INVALID_OPERATION", 0xA004 }, { "AL_OUT_OF_MEMORY", 0xA005 }, { "AL_VENDOR", 0xB001 }, { "AL_VERSION", 0xB002 },
    { "AL_RENDERER", 0xB003 }, { "AL_EXTENSIONS", 0xB004 }, { "AL_DOPPLER_FACTOR", 0xC000 }, { "AL_DOPPLER_VELOCITY", 0xC001 },
    { "AL_SPEED_OF_SOUND", 0xC003 }, { "AL_DISTANCE_MODEL", 0xD000 }, { "AL_INVERSE_DISTANCE", 0xD001 },
    { "AL_INVERSE_DISTANCE_CLAMPED", 0xD002 }, { "AL_LINEAR_DISTANCE", 0xD003 }, { "AL_LINEAR_DISTANCE_CLAMPED", 0xD004 },
    { "AL_EXPONENT_DISTANCE", 0xD005 }, { "AL_EXPONENT_DISTANCE_CLAMPED", 0xD006 },
    { "AL_FORMAT_MONO_FLOAT32", 0x10010 }, { "AL_FORMAT_STEREO_FLOAT32", 0x10011 },
    { "AL_FORMAT_QUAD8", 0x1204 }, { "AL_FORMAT_QUAD16", 0x1205 }, { "AL_FORMAT_QUAD32", 0x1206 }, { "AL_FORMAT_REAR8", 0x1207 },
    { "AL_FORMAT_REAR16", 0x1208 }, { "AL_FORMAT_REAR32", 0x1209 }, { "AL_FORMAT_51CHN8", 0x120A }, { "AL_FORMAT_51CHN16", 0x120B },
    { "AL_FORMAT_51CHN32", 0x120C }, { "AL_FORMAT_61CHN8", 0x120D }, { "AL_FORMAT_61CHN16", 0x120E }, { "AL_FORMAT_61CHN32", 0x120F },
    { "AL_FORMAT_71CHN8", 0x1210 }, { "AL_FORMAT_71CHN16", 0x1211 }, { "AL_FORMAT_71CHN32", 0x1212 },
    { "AL_FORMAT_MONO_IMA4", 0x1300 }, { "AL_FORMAT_STEREO_IMA4", 0x1301 },
    { "ALC_FALSE", 0 }, { "ALC_TRUE", 1 }, { "ALC_NO_ERROR", 0 }, { "ALC_MAJOR_VERSION", 0x1000 }, { "ALC_MINOR_VERSION", 0x1001 },
    { "ALC_ATTRIBUTES_SIZE", 0x1002 }, { "ALC_ALL_ATTRIBUTES", 0x1003 }, { "ALC_DEFAULT_DEVICE_SPECIFIER", 0x1004 },
    { "ALC_DEVICE_SPECIFIER", 0x1005 }, { "ALC_EXTENSIONS", 0x1006 }, { "ALC_FREQUENCY", 0x1007 }, { "ALC_REFRESH", 0x1008 },
    { "ALC_SYNC", 0x1009 }, { "ALC_MONO_SOURCES", 0x1010 }, { "ALC_STEREO_SOURCES", 0x1011 },
    { "ALC_DEFAULT_ALL_DEVICES_SPECIFIER", 0x1012 }, { "ALC_ALL_DEVICES_SPECIFIER", 0x1013 },
    { "ALC_CAPTURE_DEVICE_SPECIFIER", 0x310 }, { "ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER", 0x311 }, { "ALC_CAPTURE_SAMPLES", 0x312 },
    { "ALC_INVALID_DEVICE", 0xA001 }, { "ALC_INVALID_CONTEXT", 0xA002 }, { "ALC_INVALID_ENUM", 0xA003 },
    { "ALC_INVALID_VALUE", 0xA004 }, { "ALC_OUT_OF_MEMORY", 0xA005 },
};
static const char* const AL_EXTS = "AL_EXT_FLOAT32 AL_EXT_MCFORMATS AL_EXT_OFFSET AL_EXT_LINEAR_DISTANCE AL_EXT_EXPONENT_DISTANCE AL_EXT_source_distance_model";
static const char* const ALC_EXTS = "ALC_ENUMERATE_ALL_EXT ALC_ENUMERATION_EXT ALC_EXT_CAPTURE";
static const char DEVNAME[] = "Generic Software";

#define MAXSRC 256
#define MAXBUF (1u << 20)
typedef struct {
    int16_t* d; uint32_t frames, freq, bytes;   /* stored: d = frames * ch int16 */
    int ch, bits, srcch, used, refs;           /* bits/srcch: as given to alBufferData (reported by alGetBuffer) */
} Buf;
typedef struct {
    int used; uint32_t ctx;
    float pos[3], vel[3], dir[3];
    float gain, ming, maxg, pitch, ref, maxd, roll, cin, cout, coutg;
    int rel, loop, state, type, dmodel;
    uint32_t* q; int nq, capq, cur;            /* queue; q[0..cur) are processed while playing */
    uint64_t fp;                               /* 32.32 frame position in q[cur] */
    int64_t pend;                              /* offset (frames) set while not playing, applied by Play; -1 none */
    float gl, gr; int rampok;
} Src;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static Src g_src[MAXSRC];
static Buf* g_buf; static uint32_t g_nbuf, g_capbuf, g_bufnext;
static int g_err, g_alcerr;
static float g_lpos[3], g_lvel[3], g_lori[6] = { 0, 0, -1, 0, 1, 0 }, g_lgain = 1;
static float g_doppler = 1, g_dvel = 1, g_sos = 343.3f; static int g_dmodel = AL_INVERSE_DISTANCE_CLAMPED, g_srcdm;
static uint32_t g_dev[4]; static int g_ndev;
static struct { uint32_t h, dev; } g_ctx[8]; static int g_nctx; static uint32_t g_curctx;
static int g_rate = 44100, g_log = 1, g_on = -1, g_ima4, g_lite_rate, g_audio_tick_ms = 0, g_al_tick_us = 0;
static float g_master = 1.0f;
static uint64_t g_al_last_tick_ns = 0;
static const char* g_backend = "none";
static uint64_t st_frames, st_plays, st_bytes, st_bufdata; static unsigned st_errs, st_maxact, st_gensrc, st_genbuf, st_inuse;

static void al_init_once(void);
static void al_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, al_init_once); }
static void al_init_once(void) {
    const char* e = getenv("ME1_AL"); g_on = !(e && !strcmp(e, "0"));
    if ((e = getenv("ME1_AL_GAIN"))) g_master = (float)atof(e);
    if ((e = getenv("ME1_AL_RATE")) && atoi(e) >= 8000 && atoi(e) <= 192000) g_rate = atoi(e);
    if ((e = getenv("ME1_AL_LOG"))) g_log = atoi(e);
    if ((e = getenv("ME1_AL_IMA4"))) g_ima4 = atoi(e) != 0;
    /* v0.10 "simplified calculation" (ME1_SHADER_LITE=1): mix at 32 kHz instead of the device's 48 kHz (-1/3 mixing work) */
    if ((e = getenv("ME1_SHADER_LITE")) && *e == '1' && !getenv("ME1_AL_RATE")) g_lite_rate = 32000;
    /* T-3: ME1_AUDIO_TICK=ms — sleep this many ms per render block when no sources are playing */
    if ((e = getenv("ME1_AUDIO_TICK")) && atoi(e) > 0) g_audio_tick_ms = atoi(e);
    /* T-5: ME1_AL_TICK_US=us — throttle guest audio timer ticks to this interval.
     * Host mixer keeps running at full device rate; only the guest callback is throttled.
     * Default 0 = no throttling (every timer callback reaches the guest). */
    if ((e = getenv("ME1_AL_TICK_US")) && atoi(e) > 0) g_al_tick_us = atoi(e);
}
int hle_al_enabled(void) { al_init(); return g_on; }
uint32_t hle_al_module(void) { return hle_al_enabled() ? AL_HMOD : 0; }
/* T-5: return 1 if the guest audio timer callback may fire now (throttled by
 * ME1_AL_TICK_US), 0 if it should be skipped this round. Host mixer keeps
 * running at full device rate regardless. */
int hle_al_tick_due(uint64_t now_ns) {
    if (!g_al_tick_us) return 1;
    int64_t due = (int64_t)g_al_last_tick_ns + (int64_t)g_al_tick_us * 1000;
    if ((int64_t)now_ns < due) return 0;
    g_al_last_tick_ns = (int64_t)now_ns - due > (int64_t)g_al_tick_us * 1000 ? now_ns : (uint64_t)due;   /* far behind: no burst of catch-up ticks */
    return 1;
}

#define LK pthread_mutex_lock(&g_mx)
#define UL pthread_mutex_unlock(&g_mx)
static float F(CPU* c, int n) { uint32_t u = ARG(n); float f; memcpy(&f, &u, 4); return f; }
static const char* errname(int e) {
    switch (e) { case 0: return "AL_NO_ERROR"; case AL_INVALID_NAME: return "AL_INVALID_NAME"; case AL_INVALID_ENUM: return "AL_INVALID_ENUM";
    case AL_INVALID_VALUE: return "AL_INVALID_VALUE"; case AL_INVALID_OPERATION: return "AL_INVALID_OPERATION"; case AL_OUT_OF_MEMORY: return "AL_OUT_OF_MEMORY"; }
    return "?";
}
static void seterr(const char* fn, int e, uint32_t a, uint32_t b) {   /* under g_mx */
    if (!e) return;
    if (!g_err) g_err = e;
    if (st_errs++ < 40 && g_log) fprintf(stderr, "[al] %s(%#x, %#x): %s%s\n", fn, a, b, errname(e), st_errs == 40 ? " (further errors not logged)" : "");
}
static void alcerr(const char* fn, int e) {
    if (!g_alcerr) g_alcerr = e;
    if (st_errs++ < 40 && g_log) fprintf(stderr, "[al] %s: ALC error %#x\n", fn, e);
}
/* constant guest strings (allocated once per host literal) */
static uint32_t gs(const char* s, size_t n) {   /* n: bytes incl. terminator(s) */
    static const char* k[64]; static size_t kn[64]; static uint32_t v[64]; static int nk; static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mx);
    for (int i = 0; i < nk; i++) if (k[i] == s && kn[i] == n) { uint32_t r = v[i]; pthread_mutex_unlock(&mx); return r; }
    uint32_t g = ghalloc((uint32_t)n, 1); memcpy(M + g, s, n);
    if (nk < 64) { k[nk] = s; kn[nk] = n; v[nk++] = g; }
    pthread_mutex_unlock(&mx);
    return g;
}
#define GS(lit) gs(lit, sizeof(lit))
static int in_list(const char* list, const char* name) {
    size_t n = strlen(name); if (!n) return 0;
    for (const char* p = list; *p;) {
        const char* e = strchr(p, ' '); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncasecmp(p, name, n)) return 1;
        if (!e) break; p = e + 1;
    }
    return 0;
}
static int enum_value(const char* name) {
    if (!g_ima4 && strstr(name, "IMA4")) return 0;
    for (unsigned i = 0; i < sizeof g_enums / sizeof g_enums[0]; i++) if (!strcmp(g_enums[i].n, name)) return g_enums[i].v;
    return 0;
}
static void note(const char* what, const char* name, uint32_t r) {
    static char seen[64][64]; static int n; static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mx);
    char key[64]; snprintf(key, sizeof key, "%.20s:%.40s", what, name);
    int dup = 0; for (int i = 0; i < n; i++) if (!strcmp(seen[i], key)) dup = 1;
    if (!dup && n < 64) snprintf(seen[n++], 64, "%s", key);
    pthread_mutex_unlock(&mx);
    if (!dup && g_log) fprintf(stderr, "[al] %s(\"%s\") -> %#x\n", what, name, r);
}

/* ---- objects ---- */
static Src* src_get(uint32_t id) { return id >= 1 && id <= MAXSRC && g_src[id - 1].used ? &g_src[id - 1] : NULL; }
static Buf* buf_get(uint32_t id) { return id >= 1 && id <= g_nbuf && g_buf[id - 1].used ? &g_buf[id - 1] : NULL; }
static void src_defaults(Src* s, uint32_t ctx) {
    uint32_t* q = s->q; int capq = s->capq;
    memset(s, 0, sizeof *s); s->q = q; s->capq = capq;
    s->used = 1; s->ctx = ctx; s->gain = 1; s->maxg = 1; s->pitch = 1; s->ref = 1; s->maxd = FLT_MAX; s->roll = 1;
    s->cin = 360; s->cout = 360; s->state = AL_INITIAL; s->type = AL_UNDETERMINED; s->pend = -1; s->dmodel = AL_INVERSE_DISTANCE_CLAMPED;
}
static void src_clear_queue(Src* s) {
    for (int i = 0; i < s->nq; i++) { Buf* b = buf_get(s->q[i]); if (b && b->refs) b->refs--; }
    s->nq = s->cur = 0; s->fp = 0;
}
static void src_push(Src* s, uint32_t id) {
    if (s->nq == s->capq) { s->capq = s->capq ? 2 * s->capq : 8; s->q = realloc(s->q, sizeof *s->q * s->capq); }
    s->q[s->nq++] = id; Buf* b = buf_get(id); if (b) b->refs++;
}
static int src_processed(const Src* s) {
    if (s->type != AL_STREAMING || s->loop || s->state == AL_INITIAL) return 0;
    return s->cur < s->nq ? s->cur : s->nq;
}
static void src_end(Src* s) { s->state = AL_STOPPED; s->cur = s->nq; s->fp = 0; s->rampok = 0; }
static void src_play(Src* s) {
    if (!s->nq) { s->state = AL_STOPPED; s->cur = 0; s->fp = 0; return; }
    st_plays++;
    if (s->state == AL_PAUSED) { s->state = AL_PLAYING; return; }
    s->cur = 0; s->fp = 0; s->rampok = 0; s->state = AL_PLAYING;
    if (s->pend >= 0) {
        uint64_t f = (uint64_t)s->pend; s->pend = -1;
        for (int i = 0; i < s->nq; i++) { Buf* b = buf_get(s->q[i]); uint32_t n = b ? b->frames : 0; if (f < n) { s->cur = i; s->fp = f << 32; return; } f -= n; }
        s->cur = 0; s->fp = 0;
    }
}
static void src_stop(Src* s) { s->state = AL_STOPPED; s->cur = s->nq; s->fp = 0; s->pend = -1; s->rampok = 0; }
static void src_rewind(Src* s) { s->state = AL_INITIAL; s->cur = 0; s->fp = 0; s->pend = -1; s->rampok = 0; }
static uint32_t fbytes(const Buf* b) { return (uint32_t)(b->srcch * (b->bits ? b->bits : 16) / 8); }
/* playback position in frames from the start of the queue */
static uint64_t src_offset(const Src* s) {
    if (s->state != AL_PLAYING && s->state != AL_PAUSED) return s->pend >= 0 ? (uint64_t)s->pend : 0;
    uint64_t f = 0;
    for (int i = 0; i < s->cur && i < s->nq; i++) { Buf* b = buf_get(s->q[i]); if (b) f += b->frames; }
    return f + (s->fp >> 32);
}
static const Buf* src_fmt(const Src* s) { for (int i = 0; i < s->nq; i++) { Buf* b = buf_get(s->q[i]); if (b && b->freq) return b; } return NULL; }
static int src_set_offset(Src* s, uint32_t p, double v) {
    const Buf* b = src_fmt(s); if (!b || v < 0) return AL_INVALID_VALUE;
    double f = p == AL_SEC_OFFSET ? v * b->freq : p == AL_SAMPLE_OFFSET ? v : (b->bits == 4 ? v / (36.0 * b->srcch) * 65.0 : v / fbytes(b));
    uint64_t fr = (uint64_t)f;
    if (s->state != AL_PLAYING && s->state != AL_PAUSED) { s->pend = (int64_t)fr; return 0; }
    for (int i = 0; i < s->nq; i++) { Buf* q = buf_get(s->q[i]); uint32_t n = q ? q->frames : 0; if (fr < n) { s->cur = i; s->fp = fr << 32; return 0; } fr -= n; }
    return AL_INVALID_VALUE;
}
static int p_is_id(uint32_t p) { return p == AL_BUFFER; }
static int is_vec(uint32_t p) { return p == AL_POSITION || p == AL_VELOCITY || p == AL_DIRECTION; }
static int fin(double v) { return v == v && v <= DBL_MAX && v >= -DBL_MAX; }
static int src_setv(Src* s, uint32_t p, const double* v, int nv) {
    if (is_vec(p) != (nv == 3)) return AL_INVALID_ENUM;
    for (int i = 0; i < nv; i++) if (!fin(v[i])) return AL_INVALID_VALUE;
    double x = v[0];
    switch (p) {
    case AL_PITCH: if (x < 0) return AL_INVALID_VALUE; s->pitch = (float)x; return 0;
    case AL_GAIN: if (x < 0) return AL_INVALID_VALUE; s->gain = (float)x; return 0;
    case AL_MIN_GAIN: if (x < 0 || x > 1) return AL_INVALID_VALUE; s->ming = (float)x; return 0;
    case AL_MAX_GAIN: if (x < 0 || x > 1) return AL_INVALID_VALUE; s->maxg = (float)x; return 0;
    case AL_REFERENCE_DISTANCE: if (x < 0) return AL_INVALID_VALUE; s->ref = (float)x; return 0;
    case AL_MAX_DISTANCE: if (x < 0) return AL_INVALID_VALUE; s->maxd = (float)(x > FLT_MAX ? FLT_MAX : x); return 0;
    case AL_ROLLOFF_FACTOR: if (x < 0) return AL_INVALID_VALUE; s->roll = (float)x; return 0;
    case AL_CONE_INNER_ANGLE: if (x < 0 || x > 360) return AL_INVALID_VALUE; s->cin = (float)x; return 0;
    case AL_CONE_OUTER_ANGLE: if (x < 0 || x > 360) return AL_INVALID_VALUE; s->cout = (float)x; return 0;
    case AL_CONE_OUTER_GAIN: if (x < 0 || x > 1) return AL_INVALID_VALUE; s->coutg = (float)x; return 0;
    case AL_POSITION: for (int i = 0; i < 3; i++) s->pos[i] = (float)v[i]; return 0;
    case AL_VELOCITY: for (int i = 0; i < 3; i++) s->vel[i] = (float)v[i]; return 0;
    case AL_DIRECTION: for (int i = 0; i < 3; i++) s->dir[i] = (float)v[i]; return 0;
    case AL_SOURCE_RELATIVE: if (x != 0 && x != 1) return AL_INVALID_VALUE; s->rel = x != 0; return 0;
    case AL_LOOPING: if (x != 0 && x != 1) return AL_INVALID_VALUE; s->loop = x != 0; return 0;
    case AL_BUFFER: {
        uint32_t id = (uint32_t)x;
        if (s->state == AL_PLAYING || s->state == AL_PAUSED) return AL_INVALID_OPERATION;
        if (id && !buf_get(id)) return AL_INVALID_NAME;
        src_clear_queue(s);
        if (id) { src_push(s, id); s->type = AL_STATIC; } else s->type = AL_UNDETERMINED;
        return 0;
    }
    case AL_SEC_OFFSET: case AL_SAMPLE_OFFSET: case AL_BYTE_OFFSET: return src_set_offset(s, p, x);
    case AL_DISTANCE_MODEL:
        if (x != AL_NONE && (x < AL_INVERSE_DISTANCE || x > AL_EXPONENT_DISTANCE_CLAMPED)) return AL_INVALID_VALUE;
        s->dmodel = (int)x; return 0;
    case AL_SOURCE_STATE: case AL_SOURCE_TYPE: case AL_BUFFERS_QUEUED: case AL_BUFFERS_PROCESSED: return AL_INVALID_OPERATION;
    }
    return AL_INVALID_ENUM;
}
static int src_getv(const Src* s, uint32_t p, double* v) {   /* number of values, or -error */
    switch (p) {
    case AL_PITCH: v[0] = s->pitch; return 1;
    case AL_GAIN: v[0] = s->gain; return 1;
    case AL_MIN_GAIN: v[0] = s->ming; return 1;
    case AL_MAX_GAIN: v[0] = s->maxg; return 1;
    case AL_REFERENCE_DISTANCE: v[0] = s->ref; return 1;
    case AL_MAX_DISTANCE: v[0] = s->maxd; return 1;
    case AL_ROLLOFF_FACTOR: v[0] = s->roll; return 1;
    case AL_CONE_INNER_ANGLE: v[0] = s->cin; return 1;
    case AL_CONE_OUTER_ANGLE: v[0] = s->cout; return 1;
    case AL_CONE_OUTER_GAIN: v[0] = s->coutg; return 1;
    case AL_POSITION: for (int i = 0; i < 3; i++) v[i] = s->pos[i]; return 3;
    case AL_VELOCITY: for (int i = 0; i < 3; i++) v[i] = s->vel[i]; return 3;
    case AL_DIRECTION: for (int i = 0; i < 3; i++) v[i] = s->dir[i]; return 3;
    case AL_SOURCE_RELATIVE: v[0] = s->rel; return 1;
    case AL_LOOPING: v[0] = s->loop; return 1;
    case AL_BUFFER: v[0] = s->nq ? s->q[s->cur < s->nq ? s->cur : s->nq - 1] : 0; return 1;
    case AL_SOURCE_STATE: v[0] = s->state; return 1;
    case AL_SOURCE_TYPE: v[0] = s->type; return 1;
    case AL_BUFFERS_QUEUED: v[0] = s->nq; return 1;
    case AL_BUFFERS_PROCESSED: v[0] = src_processed(s); return 1;
    case AL_DISTANCE_MODEL: v[0] = s->dmodel; return 1;
    case AL_SEC_OFFSET: case AL_SAMPLE_OFFSET: case AL_BYTE_OFFSET: {
        const Buf* b = src_fmt(s); double f = (double)src_offset(s);
        if (!b) { v[0] = 0; return 1; }
        v[0] = p == AL_SEC_OFFSET ? f / b->freq : p == AL_SAMPLE_OFFSET ? f : b->bits == 4 ? floor(f / 65.0) * 36.0 * b->srcch : f * fbytes(b);
        return 1;
    }
    }
    return -AL_INVALID_ENUM;
}

/* ---- format conversion (outside the lock) ---- */
static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int ima_idx[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static void ima_block(const uint8_t* src, int ch, int16_t* dst) {   /* 36*ch bytes -> 65 frames (MS IMA ADPCM layout) */
    int smp[2], idx[2];
    for (int c = 0; c < ch; c++) {
        smp[c] = (int16_t)(src[0] | src[1] << 8); idx[c] = src[2] > 88 ? 88 : src[2]; src += 4; dst[c] = (int16_t)smp[c];
    }
    for (int j = 1; j < 65; j += 8)
        for (int c = 0; c < ch; c++) {
            uint32_t code = src[0] | src[1] << 8 | src[2] << 16 | (uint32_t)src[3] << 24; src += 4;
            for (int k = 0; k < 8; k++, code >>= 4) {
                int n = code & 15, st = ima_step[idx[c]], d = st >> 3;
                if (n & 4) d += st; if (n & 2) d += st >> 1; if (n & 1) d += st >> 2;
                smp[c] += n & 8 ? -d : d; if (smp[c] > 32767) smp[c] = 32767; if (smp[c] < -32768) smp[c] = -32768;
                idx[c] += ima_idx[n]; if (idx[c] < 0) idx[c] = 0; if (idx[c] > 88) idx[c] = 88;
                dst[(j + k) * ch + c] = (int16_t)smp[c];
            }
        }
}
static int fmt_info(uint32_t f, int* ch, int* bits) {   /* bits 8/16/32 (float) or 4 (IMA4) */
    switch (f) {
    case 0x1100: *ch = 1; *bits = 8; return 1; case 0x1101: *ch = 1; *bits = 16; return 1;
    case 0x1102: *ch = 2; *bits = 8; return 1; case 0x1103: *ch = 2; *bits = 16; return 1;
    case 0x10010: *ch = 1; *bits = 32; return 1; case 0x10011: *ch = 2; *bits = 32; return 1;
    case 0x1300: *ch = 1; *bits = 4; return g_ima4; case 0x1301: *ch = 2; *bits = 4; return g_ima4;
    }
    if (f >= 0x1204 && f <= 0x1212) {
        static const int chs[5] = { 4, 2, 6, 7, 8 };   /* QUAD, REAR, 5.1, 6.1, 7.1 */
        *ch = chs[(f - 0x1204) / 3]; *bits = (int[]){ 8, 16, 32 }[(f - 0x1204) % 3]; return 1;
    }
    return 0;
}
static float pcm(const uint8_t* p, int bits) {
    if (bits == 8) return (float)((int)p[0] - 128) * 256.0f;
    if (bits == 16) return (float)(int16_t)(p[0] | p[1] << 8);
    float f; memcpy(&f, p, 4); return f * 32767.0f;
}
static int16_t s16(float v) { v = v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v; return (int16_t)lrintf(v); }
/* returns 0 or an AL error; *out: malloc'd int16 frames (mono stays mono, everything else becomes stereo) */
static int convert(uint32_t f, const uint8_t* src, uint32_t size, int16_t** out, uint32_t* frames, int* och, int* bits, int* srcch, uint32_t* bytes) {
    int ch, b;
    if (!fmt_info(f, &ch, &b)) return AL_INVALID_ENUM;
    *bits = b; *srcch = ch; *och = ch == 1 ? 1 : 2;
    if (b == 4) {
        uint32_t blk = 36u * ch, nb = size / blk; *frames = nb * 65; *bytes = nb * blk;
        int16_t* d = malloc((size_t)*frames * ch * 2 + 4); if (!d) return AL_OUT_OF_MEMORY;
        for (uint32_t i = 0; i < nb; i++) { if (src) ima_block(src + i * blk, ch, d + (size_t)i * 65 * ch); else memset(d + (size_t)i * 65 * ch, 0, 130u * ch); }
        *out = d; return 0;
    }
    uint32_t fb = (uint32_t)(ch * b / 8), n = size / fb; *frames = n; *bytes = n * fb;
    int16_t* d = malloc((size_t)n * *och * 2 + 4); if (!d) return AL_OUT_OF_MEMORY;
    *out = d;
    if (!src) { memset(d, 0, (size_t)n * *och * 2); return 0; }
    if (ch <= 2 && b == 16) { memcpy(d, src, (size_t)n * fb); return 0; }
    int bs = b / 8;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* p = src + (size_t)i * fb; float x[8];
        for (int k = 0; k < ch; k++) x[k] = pcm(p + k * bs, b);
        float L, R;
        switch (ch) {
        case 1: d[i] = s16(x[0]); continue;
        case 2: L = x[0]; R = x[1]; break;
        case 4: L = 0.6f * (x[0] + 0.7071f * x[2]); R = 0.6f * (x[1] + 0.7071f * x[3]); break;                          /* FL FR BL BR */
        case 6: { float c = 0.7071f * x[2] + 0.5f * x[3]; L = 0.6f * (x[0] + c + 0.7071f * x[4]); R = 0.6f * (x[1] + c + 0.7071f * x[5]); break; }
        case 7: { float c = 0.7071f * x[2] + 0.5f * x[3] + 0.5f * x[4]; L = 0.6f * (x[0] + c + 0.7071f * x[5]); R = 0.6f * (x[1] + c + 0.7071f * x[6]); break; }
        default: { float c = 0.7071f * x[2] + 0.5f * x[3];                                                                 /* FL FR FC LFE BL BR SL SR */
                   L = 0.5f * (x[0] + c + 0.7071f * (x[4] + x[6])); R = 0.5f * (x[1] + c + 0.7071f * (x[5] + x[7])); break; }
        }
        d[2 * i] = s16(L); d[2 * i + 1] = s16(R);
    }
    return 0;
}

/* ---- mixer (under g_mx) ---- */
static Buf* qbuf(Src* s, int i) { Buf* b = i >= 0 && i < s->nq ? buf_get(s->q[i]) : NULL; return b && b->frames && b->d ? b : NULL; }
static Buf* src_cur(Src* s) {   /* the buffer s->fp points into, advancing over finished/empty buffers; NULL at the end */
    for (int k = 0; k <= 2 * s->nq + 64; k++) {
        if (s->cur >= s->nq) { if (!s->loop || !s->nq) return NULL; s->cur = 0; }
        Buf* b = qbuf(s, s->cur);
        if (b && (s->fp >> 32) < b->frames) return b;
        if (b) s->fp -= (uint64_t)b->frames << 32; else s->fp = 0;
        s->cur++;
    }
    return NULL;
}
static void norm3(float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void src_gains(const Src* s, const Buf* b, float* gl, float* gr) {
    float g = s->gain;
    if (b->ch != 1) { g = g < s->ming ? s->ming : g > s->maxg ? s->maxg : g; *gl = *gr = g * g_lgain; return; }
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = s->rel ? s->pos[i] : s->pos[i] - g_lpos[i];
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    int dm = g_srcdm ? s->dmodel : g_dmodel;
    float ref = s->ref, mx = s->maxd, ro = s->roll, dd = dist, att = 1;
    int clamp = dm == AL_INVERSE_DISTANCE_CLAMPED || dm == AL_LINEAR_DISTANCE_CLAMPED || dm == AL_EXPONENT_DISTANCE_CLAMPED;
    if (clamp) { if (mx < ref) dd = ref; else { if (dd < ref) dd = ref; if (dd > mx) dd = mx; } }
    switch (dm) {
    case AL_INVERSE_DISTANCE: case AL_INVERSE_DISTANCE_CLAMPED:
        if (ref > 0) { float den = ref + ro * (dd - ref); att = den > 0 ? ref / den : 1; } break;
    case AL_LINEAR_DISTANCE: case AL_LINEAR_DISTANCE_CLAMPED:
        if (mx != ref) att = 1 - ro * ((dd < mx ? dd : mx) - ref) / (mx - ref); break;
    case AL_EXPONENT_DISTANCE: case AL_EXPONENT_DISTANCE_CLAMPED:
        if (dd > 0 && ref > 0) att = powf(dd / ref, -ro); break;
    }
    if (!(att > 0)) att = 0; if (att > 1e6f) att = 1e6f;
    float cg = 1, dir[3] = { s->dir[0], s->dir[1], s->dir[2] };
    if ((s->cin < 360 || s->cout < 360) && (dir[0] != 0 || dir[1] != 0 || dir[2] != 0) && dist > 1e-6f) {
        norm3(dir);
        float dot = -(dir[0] * d[0] + dir[1] * d[1] + dir[2] * d[2]) / dist; dot = dot > 1 ? 1 : dot < -1 ? -1 : dot;
        float ang = 2.0f * acosf(dot) * (180.0f / (float)M_PI);
        if (ang > s->cin) cg = ang < s->cout && s->cout > s->cin ? 1 + (s->coutg - 1) * (ang - s->cin) / (s->cout - s->cin) : s->coutg;
    }
    g *= att * cg;
    g = g < s->ming ? s->ming : g > s->maxg ? s->maxg : g;
    g *= g_lgain;
    float x = d[0];
    if (!s->rel) {   /* listener space: right = at x up */
        float at[3] = { g_lori[0], g_lori[1], g_lori[2] }, up[3] = { g_lori[3], g_lori[4], g_lori[5] };
        float r[3] = { at[1] * up[2] - at[2] * up[1], at[2] * up[0] - at[0] * up[2], at[0] * up[1] - at[1] * up[0] };
        norm3(r); x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
    }
    float pan = dist > 1e-5f ? x / dist : 0; pan = pan > 1 ? 1 : pan < -1 ? -1 : pan;
    float a = (pan + 1) * (float)M_PI / 4;
    *gl = g * cosf(a); *gr = g * sinf(a);
}
static void mix_src(Src* s, float* mix, int n) {
    Buf* b = src_cur(s);
    if (!b) { src_end(s); return; }
    float tl, tr; src_gains(s, b, &tl, &tr);
    if (!s->rampok) { s->gl = tl; s->gr = tr; s->rampok = 1; }
    float gl = s->gl, gr = s->gr, dl = (tl - gl) / n, dr = (tr - gr) / n;
    float pitch = s->pitch < 1e-3f ? 1e-3f : s->pitch > 16 ? 16 : s->pitch;
    int i = 0;
    while (i < n) {
        uint64_t st = (uint64_t)((double)b->freq * pitch / g_rate * 4294967296.0); if (!st) st = 1;
        const int16_t* d = b->d; uint32_t fr = b->frames; int ch = b->ch;
        int nx = s->cur + 1; if (nx >= s->nq) nx = s->loop ? 0 : -1;
        Buf* nb = qbuf(s, nx); const int16_t* nd = nb && nb->ch == ch ? nb->d : NULL;
        for (; i < n; i++) {
            uint32_t idx = (uint32_t)(s->fp >> 32);
            if (idx >= fr) break;
            float f = (float)(uint32_t)s->fp * (1.0f / 4294967296.0f);
            if (ch == 1) {
                float a = d[idx], c = idx + 1 < fr ? d[idx + 1] : nd ? nd[0] : a, v = a + (c - a) * f;
                mix[2 * i] += v * gl; mix[2 * i + 1] += v * gr;
            } else {
                float a0 = d[2 * idx], a1 = d[2 * idx + 1], c0 = a0, c1 = a1;
                if (idx + 1 < fr) { c0 = d[2 * idx + 2]; c1 = d[2 * idx + 3]; } else if (nd) { c0 = nd[0]; c1 = nd[1]; }
                mix[2 * i] += (a0 + (c0 - a0) * f) * gl; mix[2 * i + 1] += (a1 + (c1 - a1) * f) * gr;
            }
            gl += dl; gr += dr; s->fp += st;
        }
        if (i < n && !(b = src_cur(s))) { src_end(s); break; }
    }
    s->gl = tl; s->gr = tr;
}
#define MIXN 1024
static void ds_mix(float* mix, int n);
void hle_al_render(int16_t* out, int frames) {
    float mix[2 * MIXN];
    while (frames > 0) {
        int n = frames > MIXN ? MIXN : frames, act = 0;
        memset(mix, 0, sizeof(float) * 2 * n);
        LK;
        for (int k = 0; k < MAXSRC; k++) if (g_src[k].used && g_src[k].state == AL_PLAYING) { mix_src(&g_src[k], mix, n); act++; }
        ds_mix(mix, n);
        st_frames += (uint64_t)n; if ((unsigned)act > st_maxact) st_maxact = (unsigned)act;
        UL;
        /* T-3: no active sources — sleep instead of spinning, so the host core
         * can idle instead of burning cycles rendering silence. */
        if (act == 0 && g_audio_tick_ms > 0) usleep((useconds_t)g_audio_tick_ms * 1000);
        for (int i = 0; i < 2 * n; i++) out[i] = s16(mix[i] * g_master);
        out += 2 * n; frames -= n;
    }
}
int hle_al_rate(void) { return g_rate; }

/* ---- output: AAudio or the null sink ---- */
typedef struct AAudioStreamBuilderStruct AAB; typedef struct AAudioStreamStruct AAS;
static struct {
    int32_t (*create)(AAB**); void (*fmt)(AAB*, int32_t); void (*chans)(AAB*, int32_t); void (*rate)(AAB*, int32_t);
    void (*share)(AAB*, int32_t); void (*perf)(AAB*, int32_t); void (*usage)(AAB*, int32_t); void (*ctype)(AAB*, int32_t);
    void (*datacb)(AAB*, int32_t (*)(AAS*, void*, void*, int32_t), void*); void (*errcb)(AAB*, void (*)(AAS*, void*, int32_t), void*);
    int32_t (*open)(AAB*, AAS**); int32_t (*bdel)(AAB*);
    int32_t (*start)(AAS*); int32_t (*stop)(AAS*); int32_t (*close)(AAS*); int32_t (*getrate)(AAS*); int32_t (*burst)(AAS*);
    int32_t (*setbuf)(AAS*, int32_t); int32_t (*cap)(AAS*); int32_t (*xruns)(AAS*); int32_t (*getfmt)(AAS*); int32_t (*getch)(AAS*);
} AA;
static AAS* g_aas; static int g_aa_restarting; static unsigned st_restarts;
static int32_t aa_data(AAS* s, void* u, void* data, int32_t n) { (void)s; (void)u; hle_al_render((int16_t*)data, n); return 0; }
static int aa_open(void);
static void* aa_restart(void* p) {
    (void)p; usleep(200000);
    if (g_aas) { AA.stop(g_aas); AA.close(g_aas); g_aas = NULL; }
    st_restarts++;
    if (!aa_open()) fprintf(stderr, "[al] AAudio reopen failed: no sound output\n");
    __atomic_store_n(&g_aa_restarting, 0, __ATOMIC_RELEASE);
    return NULL;
}
static void aa_error(AAS* s, void* u, int32_t err) {
    (void)s; (void)u;
    fprintf(stderr, "[al] AAudio stream error %d: reopening\n", err);
    if (__atomic_exchange_n(&g_aa_restarting, 1, __ATOMIC_ACQ_REL)) return;
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, aa_restart, NULL)) __atomic_store_n(&g_aa_restarting, 0, __ATOMIC_RELEASE);
}
static int aa_load(void) {
    static int r = -1; if (r >= 0) return r;
    void* h = dlopen("libaaudio.so", RTLD_NOW); r = 0;
    if (!h) return 0;
#define S(f, n) *(void**)&AA.f = dlsym(h, n)
    S(create, "AAudio_createStreamBuilder"); S(fmt, "AAudioStreamBuilder_setFormat"); S(chans, "AAudioStreamBuilder_setChannelCount");
    S(rate, "AAudioStreamBuilder_setSampleRate"); S(share, "AAudioStreamBuilder_setSharingMode"); S(perf, "AAudioStreamBuilder_setPerformanceMode");
    S(usage, "AAudioStreamBuilder_setUsage"); S(ctype, "AAudioStreamBuilder_setContentType");
    S(datacb, "AAudioStreamBuilder_setDataCallback"); S(errcb, "AAudioStreamBuilder_setErrorCallback");
    S(open, "AAudioStreamBuilder_openStream"); S(bdel, "AAudioStreamBuilder_delete"); S(start, "AAudioStream_requestStart");
    S(stop, "AAudioStream_requestStop"); S(close, "AAudioStream_close"); S(getrate, "AAudioStream_getSampleRate");
    S(burst, "AAudioStream_getFramesPerBurst"); S(setbuf, "AAudioStream_setBufferSizeInFrames"); S(cap, "AAudioStream_getBufferCapacityInFrames");
    S(xruns, "AAudioStream_getXRunCount"); S(getfmt, "AAudioStream_getFormat"); S(getch, "AAudioStream_getChannelCount");
#undef S
    r = AA.create && AA.fmt && AA.chans && AA.datacb && AA.errcb && AA.open && AA.bdel && AA.start && AA.stop && AA.close && AA.getrate;
    return r;
}
static int aa_open(void) {
    if (!aa_load()) return 0;
    AAB* b = NULL; if (AA.create(&b) || !b) return 0;
    AA.fmt(b, 1 /* AAUDIO_FORMAT_PCM_I16 */); AA.chans(b, 2);
    if (AA.share) AA.share(b, 1 /* SHARED */);
    if (AA.perf) AA.perf(b, 10 /* PERFORMANCE_MODE_NONE */);
    if (AA.usage) AA.usage(b, 14 /* USAGE_GAME */);
    if (AA.ctype) AA.ctype(b, 2 /* CONTENT_TYPE_MUSIC */);
    const char* e = getenv("ME1_AL_RATE"); if (e && AA.rate) AA.rate(b, atoi(e)); else if (g_lite_rate && AA.rate) AA.rate(b, g_lite_rate);
    AA.datacb(b, aa_data, NULL); AA.errcb(b, aa_error, NULL);
    AAS* s = NULL; int32_t r = AA.open(b, &s); AA.bdel(b);
    if (r || !s) { fprintf(stderr, "[al] AAudio openStream failed (%d)\n", r); return 0; }
    if ((AA.getfmt && AA.getfmt(s) != 1) || (AA.getch && AA.getch(s) != 2)) { fprintf(stderr, "[al] AAudio: unexpected stream format\n"); AA.close(s); return 0; }
    int rate = AA.getrate(s); if (rate >= 8000) { LK; g_rate = rate; UL; }
    int burst = AA.burst ? AA.burst(s) : 0;
    if (AA.setbuf && burst > 0) { int want = burst * 4; while (want < rate / 25) want += burst; AA.setbuf(s, want); }   /* >= 40 ms */
    g_aas = s;
    if (AA.start(s)) { fprintf(stderr, "[al] AAudio requestStart failed\n"); AA.close(s); g_aas = NULL; return 0; }
    g_backend = "AAudio";
    fprintf(stderr, "[al] output: AAudio %d Hz stereo s16, burst %d frames, capacity %d\n", rate, burst, AA.cap ? AA.cap(s) : 0);
    return 1;
}
static FILE* g_wav; static uint64_t g_wavframes;
static void wav_header(void) {
    uint32_t data = (uint32_t)(g_wavframes * 4), h[11] = { 0x46464952u, 36 + data, 0x45564157u, 0x20746d66u, 16, 1 | 2 << 16, (uint32_t)g_rate,
        (uint32_t)g_rate * 4, 4 | 16 << 16, 0x61746164u, data };
    fseek(g_wav, 0, SEEK_SET); fwrite(h, 4, 11, g_wav); fseek(g_wav, 0, SEEK_END); fflush(g_wav);
}
static void* sink_main(void* p) {
    (void)p;
    int per = g_rate / 100; int16_t* b = malloc((size_t)per * 4);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    for (unsigned k = 0;; k++) {
        hle_al_render(b, per);
        if (g_wav) { fwrite(b, 4, (size_t)per, g_wav); g_wavframes += (uint64_t)per; if (k % 100 == 99) wav_header(); }
        t.tv_nsec += 10000000; if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
        struct timespec nw; clock_gettime(CLOCK_MONOTONIC, &nw);
        if (nw.tv_sec > t.tv_sec + 1) t = nw;   /* fell far behind (suspended): resync */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    }
    return NULL;
}
/* v1.1 (Linux port): the front end's audio device (SDL2). open(wanted rate) -> the device rate (0: none); the device
 * pulls with hle_al_render from its own thread once start() runs. */
int (*hle_al_out_open)(int rate, const char** name);
void (*hle_al_out_start)(void);
static int ext_open(void) {
    if (!hle_al_out_open) return 0;
    const char* e = getenv("ME1_AL_RATE"); const char* nm = "external";
    int want = e && atoi(e) >= 8000 ? atoi(e) : g_lite_rate ? g_lite_rate : 48000;
    int rate = hle_al_out_open(want, &nm);
    if (rate < 8000) { fprintf(stderr, "[al] %s audio device not opened\n", nm); return 0; }
    LK; g_rate = rate; UL;
    g_backend = nm;
    fprintf(stderr, "[al] output: %s %d Hz stereo s16\n", nm, rate);
    if (hle_al_out_start) hle_al_out_start();
    return 1;
}
static void out_start(void) {
    static int started; if (started) return; started = 1;
    const char* sink = getenv("ME1_AL_SINK");
    if (sink && !strcmp(sink, "manual")) { g_backend = "manual"; fprintf(stderr, "[al] output: manual (hle_al_render), %d Hz\n", g_rate); return; }
    if (!(sink && !strcmp(sink, "null")) && (ext_open() || aa_open())) return;
    const char* w = getenv("ME1_AL_WAV");
    if (w && *w) { g_wav = fopen(w, "wb"); if (g_wav) wav_header(); else fprintf(stderr, "[al] cannot write %s\n", w); }
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, sink_main, NULL)) { fprintf(stderr, "[al] cannot start the mixer thread\n"); return; }
    g_backend = g_wav ? "null+wav" : "null";
    fprintf(stderr, "[al] output: null sink %d Hz (no audio device%s%s)\n", g_rate, g_wav ? "; recording to " : "", g_wav ? w : "");
}

/* ================= ALC ================= */
static int dev_ok(uint32_t d) { for (int i = 0; i < g_ndev; i++) if (g_dev[i] == d) return 1; return 0; }
static int ctx_idx(uint32_t h) { for (int i = 0; i < g_nctx; i++) if (g_ctx[i].h == h) return i; return -1; }
static void alc_OpenDevice(CPU* c) {
    al_init();
    const char* nm = ARG(0) ? GSTR(ARG(0)) : NULL;
    if (!g_on) { fprintf(stderr, "[al] alcOpenDevice(%s) -> NULL (ME1_AL=0)\n", nm ? nm : "default"); RET(0); return; }
    LK; uint32_t h = 0;
    if (g_ndev < 4) { h = ghalloc(16, 1); wr32(h, 0x56454C41u); g_dev[g_ndev++] = h; } else alcerr("alcOpenDevice", AL_OUT_OF_MEMORY);
    UL;
    fprintf(stderr, "[al] alcOpenDevice(%s%s%s) -> %08x (%s)\n", nm ? "\"" : "", nm ? nm : "default", nm ? "\"" : "", h, DEVNAME);
    if (h) out_start();
    RET(h);
}
static void alc_CloseDevice(CPU* c) {
    LK; int ok = 0;
    for (int i = 0; i < g_ndev; i++) if (g_dev[i] == ARG(0)) { g_dev[i] = g_dev[--g_ndev]; ok = 1; break; }
    if (!ok) alcerr("alcCloseDevice", ALC_INVALID_DEVICE);
    UL; if (ok) fprintf(stderr, "[al] alcCloseDevice(%08x)\n", ARG(0));
    RET(ok);
}
static void alc_CreateContext(CPU* c) {
    uint32_t d = ARG(0), at = ARG(1), h = 0;
    LK;
    if (!dev_ok(d)) alcerr("alcCreateContext", ALC_INVALID_DEVICE);
    else if (g_nctx >= 8) alcerr("alcCreateContext", AL_OUT_OF_MEMORY);
    else { h = ghalloc(16, 1); wr32(h, 0x58544341u); g_ctx[g_nctx].h = h; g_ctx[g_nctx].dev = d; g_nctx++; }
    UL;
    char attrs[200] = ""; size_t L = 0;
    for (int i = 0; at && i < 16 && rd32(at + 8 * i) && L < 180; i++) L += (size_t)snprintf(attrs + L, sizeof attrs - L, " %#x=%d", rd32(at + 8 * i), (int)rd32(at + 8 * i + 4));
    fprintf(stderr, "[al] alcCreateContext(%08x,%s) -> %08x\n", d, L ? attrs : " no attributes", h);
    RET(h);
}
static void alc_MakeContextCurrent(CPU* c) {
    LK; int ok = 1;
    if (ARG(0) && ctx_idx(ARG(0)) < 0) { alcerr("alcMakeContextCurrent", ALC_INVALID_CONTEXT); ok = 0; } else g_curctx = ARG(0);
    UL; RET(ok);
}
static void alc_DestroyContext(CPU* c) {
    LK; int i = ctx_idx(ARG(0));
    if (i < 0) alcerr("alcDestroyContext", ALC_INVALID_CONTEXT);
    else {
        for (int k = 0; k < MAXSRC; k++) if (g_src[k].used && g_src[k].ctx == ARG(0)) { src_clear_queue(&g_src[k]); g_src[k].used = 0; }
        if (g_curctx == ARG(0)) g_curctx = 0;
        g_ctx[i] = g_ctx[--g_nctx];
    }
    UL; RET(0);
}
static void alc_GetCurrentContext(CPU* c) { LK; uint32_t h = g_curctx; UL; RET(h); }
static void alc_GetContextsDevice(CPU* c) { LK; int i = ctx_idx(ARG(0)); uint32_t d = i >= 0 ? g_ctx[i].dev : 0; if (i < 0) alcerr("alcGetContextsDevice", ALC_INVALID_CONTEXT); UL; RET(d); }
static void alc_ProcessContext(CPU* c) { (void)c; }
static void alc_SuspendContext(CPU* c) { (void)c; }
static void alc_GetError(CPU* c) { LK; int e = g_alcerr; g_alcerr = 0; UL; RET(e); }
static void alc_IsExtensionPresent(CPU* c) {
    const char* n = ARG(1) ? GSTR(ARG(1)) : ""; int r = in_list(ALC_EXTS, n);
    note("alcIsExtensionPresent", n, (uint32_t)r); RET(r);
}
static void alc_GetProcAddress(CPU* c) {
    const char* n = ARG(1) ? GSTR(ARG(1)) : ""; uint32_t t = hle_resolve_impl("OpenAL32.dll", n);
    note("alcGetProcAddress", n, t); RET(t);
}
static void alc_GetEnumValue(CPU* c) { const char* n = ARG(1) ? GSTR(ARG(1)) : ""; int v = enum_value(n); note("alcGetEnumValue", n, (uint32_t)v); RET(v); }
static void alc_GetString(CPU* c) {
    uint32_t d = ARG(0), p = ARG(1), r = 0;
    switch (p) {
    case 0: r = GS("No Error"); break;
    case ALC_INVALID_DEVICE: r = GS("Invalid Device"); break;
    case ALC_INVALID_CONTEXT: r = GS("Invalid Context"); break;
    case ALC_INVALID_ENUM: r = GS("Invalid Enum"); break;
    case ALC_INVALID_VALUE: r = GS("Invalid Value"); break;
    case 0xA005: r = GS("Out of Memory"); break;
    case ALC_DEFAULT_DEVICE_SPECIFIER: case ALC_DEFAULT_ALL_DEVICES_SPECIFIER: r = GS(DEVNAME); break;
    case ALC_DEVICE_SPECIFIER: case ALC_ALL_DEVICES_SPECIFIER:
        r = d ? GS(DEVNAME) : gs("Generic Software\0", sizeof "Generic Software\0"); break;   /* list: double-NUL terminated */
    case ALC_CAPTURE_DEVICE_SPECIFIER: r = d ? GS("") : gs("\0", 2); break;
    case ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER: r = GS(""); break;
    case ALC_EXTENSIONS: r = gs(ALC_EXTS, strlen(ALC_EXTS) + 1); break;
    default: LK; alcerr("alcGetString", ALC_INVALID_ENUM); UL;
    }
    if (g_log > 1) fprintf(stderr, "[al] alcGetString(%08x, %#x) -> \"%s\"\n", d, p, r ? GSTR(r) : "(null)");
    RET(r);
}
static void alc_GetIntegerv(CPU* c) {
    uint32_t d = ARG(0), p = ARG(1), out = ARG(3); int n = (int)ARG(2);
    if (!out || n <= 0) { LK; alcerr("alcGetIntegerv", ALC_INVALID_VALUE); UL; return; }
    int mono = MAXSRC - 32, stereo = 32;
    int attrs[11] = { ALC_FREQUENCY, g_rate, ALC_REFRESH, 50, ALC_SYNC, 0, ALC_MONO_SOURCES, mono, ALC_STEREO_SOURCES, stereo, 0 };
    switch (p) {
    case ALC_MAJOR_VERSION: wr32(out, 1); break;
    case ALC_MINOR_VERSION: wr32(out, 1); break;
    case ALC_ATTRIBUTES_SIZE: wr32(out, 11); break;
    case ALC_ALL_ATTRIBUTES:
        if (n < 11) { LK; alcerr("alcGetIntegerv", ALC_INVALID_VALUE); UL; break; }
        for (int i = 0; i < 11; i++) wr32(out + 4 * i, (uint32_t)attrs[i]);
        break;
    case ALC_FREQUENCY: wr32(out, (uint32_t)g_rate); break;
    case ALC_REFRESH: wr32(out, 50); break;
    case ALC_SYNC: wr32(out, 0); break;
    case ALC_MONO_SOURCES: wr32(out, (uint32_t)mono); break;
    case ALC_STEREO_SOURCES: wr32(out, (uint32_t)stereo); break;
    case ALC_CAPTURE_SAMPLES: wr32(out, 0); break;
    default: LK; alcerr("alcGetIntegerv", ALC_INVALID_ENUM); UL;
    }
    if (g_log > 1) fprintf(stderr, "[al] alcGetIntegerv(%08x, %#x, %d) -> %d\n", d, p, n, (int)rd32(out));
}
static void alc_CaptureOpenDevice(CPU* c) { fprintf(stderr, "[al] alcCaptureOpenDevice(%s) -> NULL (no capture)\n", ARG(0) ? GSTR(ARG(0)) : "default"); RET(0); }
static void alc_CaptureCloseDevice(CPU* c) { RET(0); }
static void alc_Nop(CPU* c) { (void)c; }

/* ================= AL: state ================= */
static void al_GetError(CPU* c) { LK; int e = g_err; g_err = 0; UL; RET(e); }
static void al_Enable(CPU* c) { LK; if (ARG(0) == AL_SOURCE_DISTANCE_MODEL) g_srcdm = 1; else seterr("alEnable", AL_INVALID_ENUM, ARG(0), 0); UL; }
static void al_Disable(CPU* c) { LK; if (ARG(0) == AL_SOURCE_DISTANCE_MODEL) g_srcdm = 0; else seterr("alDisable", AL_INVALID_ENUM, ARG(0), 0); UL; }
static void al_IsEnabled(CPU* c) { LK; int r = 0; if (ARG(0) == AL_SOURCE_DISTANCE_MODEL) r = g_srcdm; else seterr("alIsEnabled", AL_INVALID_ENUM, ARG(0), 0); UL; RET(r); }
static int state_get(const char* fn, uint32_t p, double* v) {   /* under g_mx */
    switch (p) {
    case AL_DOPPLER_FACTOR: *v = g_doppler; return 1;
    case AL_DOPPLER_VELOCITY: *v = g_dvel; return 1;
    case AL_SPEED_OF_SOUND: *v = g_sos; return 1;
    case AL_DISTANCE_MODEL: *v = g_dmodel; return 1;
    }
    seterr(fn, AL_INVALID_ENUM, p, 0); *v = 0; return 0;
}
static void al_GetBoolean(CPU* c) { double v; LK; state_get("alGetBoolean", ARG(0), &v); UL; RET(v != 0); }
static void al_GetInteger(CPU* c) { double v; LK; state_get("alGetInteger", ARG(0), &v); UL; RET((int32_t)v); }
static void al_GetFloat(CPU* c) { double v; LK; state_get("alGetFloat", ARG(0), &v); UL; fpush(c, (double)(float)v); }
static void al_GetDouble(CPU* c) { double v; LK; state_get("alGetDouble", ARG(0), &v); UL; fpush(c, v); }
static void al_GetBooleanv(CPU* c) { double v; LK; int ok = state_get("alGetBooleanv", ARG(0), &v); UL; if (ok && ARG(1)) wr8(ARG(1), v != 0); }
static void al_GetIntegerv(CPU* c) { double v; LK; int ok = state_get("alGetIntegerv", ARG(0), &v); UL; if (ok && ARG(1)) wr32(ARG(1), (uint32_t)(int32_t)v); }
static void al_GetFloatv(CPU* c) { double v; LK; int ok = state_get("alGetFloatv", ARG(0), &v); UL; if (ok && ARG(1)) wrf32(ARG(1), (float)v); }
static void al_GetDoublev(CPU* c) { double v; LK; int ok = state_get("alGetDoublev", ARG(0), &v); UL; if (ok && ARG(1)) wrf64(ARG(1), v); }
static void al_DopplerFactor(CPU* c) { float f = F(c, 0); LK; if (f >= 0 && f < FLT_MAX) g_doppler = f; else seterr("alDopplerFactor", AL_INVALID_VALUE, 0, 0); UL; }
static void al_DopplerVelocity(CPU* c) { float f = F(c, 0); LK; if (f > 0 && f < FLT_MAX) g_dvel = f; else seterr("alDopplerVelocity", AL_INVALID_VALUE, 0, 0); UL; }
static void al_SpeedOfSound(CPU* c) { float f = F(c, 0); LK; if (f > 0 && f < FLT_MAX) g_sos = f; else seterr("alSpeedOfSound", AL_INVALID_VALUE, 0, 0); UL; }
static void al_DistanceModel(CPU* c) {
    uint32_t m = ARG(0); LK;
    if (m == AL_NONE || (m >= AL_INVERSE_DISTANCE && m <= AL_EXPONENT_DISTANCE_CLAMPED)) g_dmodel = (int)m; else seterr("alDistanceModel", AL_INVALID_VALUE, m, 0);
    UL;
}
static void al_GetString(CPU* c) {
    uint32_t p = ARG(0), r = 0;
    switch (p) {
    case AL_VENDOR: r = GS("Creative Labs Inc."); break;
    case AL_VERSION: r = GS("1.1"); break;
    case AL_RENDERER: r = GS("Software"); break;
    case AL_EXTENSIONS: r = g_ima4 ? GS("AL_EXT_FLOAT32 AL_EXT_MCFORMATS AL_EXT_OFFSET AL_EXT_LINEAR_DISTANCE AL_EXT_EXPONENT_DISTANCE AL_EXT_source_distance_model AL_EXT_IMA4")
                                   : gs(AL_EXTS, strlen(AL_EXTS) + 1); break;
    case 0: r = GS("No Error"); break;
    case AL_INVALID_NAME: r = GS("Invalid Name"); break;
    case AL_INVALID_ENUM: r = GS("Invalid Enum"); break;
    case AL_INVALID_VALUE: r = GS("Invalid Value"); break;
    case AL_INVALID_OPERATION: r = GS("Invalid Operation"); break;
    case AL_OUT_OF_MEMORY: r = GS("Out of Memory"); break;
    default: LK; seterr("alGetString", AL_INVALID_ENUM, p, 0); UL;
    }
    RET(r);
}
static void al_IsExtensionPresent(CPU* c) {
    const char* n = ARG(0) ? GSTR(ARG(0)) : "";
    int r = in_list(AL_EXTS, n) || (g_ima4 && !strcasecmp(n, "AL_EXT_IMA4"));
    note("alIsExtensionPresent", n, (uint32_t)r); RET(r);
}
static void al_GetProcAddress(CPU* c) {
    const char* n = ARG(0) ? GSTR(ARG(0)) : ""; uint32_t t = hle_resolve_impl("OpenAL32.dll", n);
    note("alGetProcAddress", n, t); RET(t);
}
static void al_GetEnumValue(CPU* c) { const char* n = ARG(0) ? GSTR(ARG(0)) : ""; int v = enum_value(n); note("alGetEnumValue", n, (uint32_t)v); RET(v); }

/* ================= AL: listener ================= */
static void lis_set(const char* fn, uint32_t p, const double* v, int nv) {   /* under g_mx */
    for (int i = 0; i < nv; i++) if (!fin(v[i])) { seterr(fn, AL_INVALID_VALUE, p, 0); return; }
    if (p == AL_GAIN && nv == 1) { if (v[0] < 0) seterr(fn, AL_INVALID_VALUE, p, 0); else g_lgain = (float)v[0]; return; }
    if (p == AL_POSITION && nv == 3) { for (int i = 0; i < 3; i++) g_lpos[i] = (float)v[i]; return; }
    if (p == AL_VELOCITY && nv == 3) { for (int i = 0; i < 3; i++) g_lvel[i] = (float)v[i]; return; }
    if (p == AL_ORIENTATION && nv == 6) { for (int i = 0; i < 6; i++) g_lori[i] = (float)v[i]; return; }
    seterr(fn, AL_INVALID_ENUM, p, 0);
}
static int lis_get(uint32_t p, double* v) {
    if (p == AL_GAIN) { v[0] = g_lgain; return 1; }
    if (p == AL_POSITION) { for (int i = 0; i < 3; i++) v[i] = g_lpos[i]; return 3; }
    if (p == AL_VELOCITY) { for (int i = 0; i < 3; i++) v[i] = g_lvel[i]; return 3; }
    if (p == AL_ORIENTATION) { for (int i = 0; i < 6; i++) v[i] = g_lori[i]; return 6; }
    return 0;
}
static int lis_count(uint32_t p) { return p == AL_GAIN ? 1 : p == AL_ORIENTATION ? 6 : p == AL_POSITION || p == AL_VELOCITY ? 3 : 0; }
static void al_Listenerf(CPU* c) { double v = F(c, 1); LK; lis_set("alListenerf", ARG(0), &v, 1); UL; }
static void al_Listener3f(CPU* c) { double v[3] = { F(c, 1), F(c, 2), F(c, 3) }; LK; lis_set("alListener3f", ARG(0), v, 3); UL; }
static void al_Listenerfv(CPU* c) {
    uint32_t p = ARG(0), a = ARG(1); int n = lis_count(p); double v[6] = { 0 };
    if (!a && n) { LK; seterr("alListenerfv", AL_INVALID_VALUE, p, 0); UL; return; }
    for (int i = 0; i < n; i++) v[i] = rdf32(a + 4 * i);
    LK; if (n) lis_set("alListenerfv", p, v, n); else seterr("alListenerfv", AL_INVALID_ENUM, p, 0); UL;
}
static void al_Listeneri(CPU* c) { double v = (int32_t)ARG(1); LK; lis_set("alListeneri", ARG(0), &v, 1); UL; }
static void al_Listener3i(CPU* c) { double v[3] = { (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3) }; LK; lis_set("alListener3i", ARG(0), v, 3); UL; }
static void al_Listeneriv(CPU* c) {
    uint32_t p = ARG(0), a = ARG(1); int n = lis_count(p); double v[6] = { 0 };
    if (!a && n) { LK; seterr("alListeneriv", AL_INVALID_VALUE, p, 0); UL; return; }
    for (int i = 0; i < n; i++) v[i] = (int32_t)rd32(a + 4 * i);
    LK; if (n) lis_set("alListeneriv", p, v, n); else seterr("alListeneriv", AL_INVALID_ENUM, p, 0); UL;
}
static void lis_out(CPU* c, const char* fn, int want, int ints) {   /* want: 1 scalar, 3 vector, 0 any (fv/iv) */
    uint32_t p = ARG(0); double v[6]; LK; int n = lis_get(p, v);
    if (!n || (want && n != want)) { seterr(fn, AL_INVALID_ENUM, p, 0); UL; return; }
    UL;
    int k = want ? want : n;
    for (int i = 0; i < k; i++) { uint32_t a = want == 3 ? ARG(1 + i) : ARG(1) + 4 * i; if (!a) continue; if (ints) wr32(a, (uint32_t)(int32_t)v[i]); else wrf32(a, (float)v[i]); }
}
static void al_GetListenerf(CPU* c) { lis_out(c, "alGetListenerf", 1, 0); }
static void al_GetListener3f(CPU* c) { lis_out(c, "alGetListener3f", 3, 0); }
static void al_GetListenerfv(CPU* c) { lis_out(c, "alGetListenerfv", 0, 0); }
static void al_GetListeneri(CPU* c) { lis_out(c, "alGetListeneri", 1, 1); }
static void al_GetListener3i(CPU* c) { lis_out(c, "alGetListener3i", 3, 1); }
static void al_GetListeneriv(CPU* c) { lis_out(c, "alGetListeneriv", 0, 1); }

/* ================= AL: sources ================= */
static void al_GenSources(CPU* c) {
    int n = (int)ARG(0); uint32_t out = ARG(1);
    if (n <= 0) { if (n < 0) { LK; seterr("alGenSources", AL_INVALID_VALUE, (uint32_t)n, 0); UL; } return; }
    LK;
    int fr = 0; for (int k = 0; k < MAXSRC; k++) fr += !g_src[k].used;
    if (fr < n || !out) { seterr("alGenSources", fr < n ? AL_OUT_OF_MEMORY : AL_INVALID_VALUE, (uint32_t)n, (uint32_t)fr); UL; return; }
    for (int k = 0, i = 0; k < MAXSRC && i < n; k++) if (!g_src[k].used) { src_defaults(&g_src[k], g_curctx); wr32(out + 4 * i++, (uint32_t)k + 1); }
    st_gensrc += (unsigned)n;
    UL;
}
static void al_DeleteSources(CPU* c) {
    int n = (int)ARG(0); uint32_t ids = ARG(1);
    LK;
    for (int i = 0; i < n; i++) if (!src_get(rd32(ids + 4 * i))) { seterr("alDeleteSources", AL_INVALID_NAME, rd32(ids + 4 * i), 0); UL; return; }
    for (int i = 0; i < n; i++) { Src* s = src_get(rd32(ids + 4 * i)); if (s) { src_clear_queue(s); s->used = 0; s->state = AL_INITIAL; } }
    UL;
}
static void al_IsSource(CPU* c) { LK; int r = src_get(ARG(0)) != NULL; UL; RET(r); }
static void src_set_api(const char* fn, uint32_t id, uint32_t p, const double* v, int nv) {
    LK; Src* s = src_get(id);
    if (!s) seterr(fn, AL_INVALID_NAME, id, p); else seterr(fn, src_setv(s, p, v, nv), id, p);
    UL;
}
static void al_Sourcef(CPU* c) { double v = F(c, 2); src_set_api("alSourcef", ARG(0), ARG(1), &v, 1); }
static void al_Source3f(CPU* c) { double v[3] = { F(c, 2), F(c, 3), F(c, 4) }; src_set_api("alSource3f", ARG(0), ARG(1), v, 3); }
static void al_Sourcefv(CPU* c) {
    uint32_t p = ARG(1), a = ARG(2); int n = is_vec(p) ? 3 : 1; double v[3];
    if (!a) { LK; seterr("alSourcefv", AL_INVALID_VALUE, ARG(0), p); UL; return; }
    for (int i = 0; i < n; i++) v[i] = rdf32(a + 4 * i);
    src_set_api("alSourcefv", ARG(0), p, v, n);
}
static void al_Sourcei(CPU* c) { double v = p_is_id(ARG(1)) ? (double)ARG(2) : (double)(int32_t)ARG(2); src_set_api("alSourcei", ARG(0), ARG(1), &v, 1); }
static void al_Source3i(CPU* c) { double v[3] = { (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4) }; src_set_api("alSource3i", ARG(0), ARG(1), v, 3); }
static void al_Sourceiv(CPU* c) {
    uint32_t p = ARG(1), a = ARG(2); int n = is_vec(p) ? 3 : 1; double v[3];
    if (!a) { LK; seterr("alSourceiv", AL_INVALID_VALUE, ARG(0), p); UL; return; }
    for (int i = 0; i < n; i++) v[i] = p_is_id(p) ? (double)rd32(a) : (double)(int32_t)rd32(a + 4 * i);
    src_set_api("alSourceiv", ARG(0), p, v, n);
}
static int src_get_api(const char* fn, uint32_t id, uint32_t p, double* v) {   /* count or 0 (error set) */
    LK; Src* s = src_get(id); int n;
    if (!s) { seterr(fn, AL_INVALID_NAME, id, p); n = 0; }
    else if ((n = src_getv(s, p, v)) < 0) { seterr(fn, -n, id, p); n = 0; }
    UL; return n;
}
static void al_GetSourcef(CPU* c) { double v[3]; int n = src_get_api("alGetSourcef", ARG(0), ARG(1), v); if (n == 1 && ARG(2)) wrf32(ARG(2), (float)v[0]); else if (n == 3) { LK; seterr("alGetSourcef", AL_INVALID_ENUM, ARG(0), ARG(1)); UL; } }
static void al_GetSource3f(CPU* c) { double v[3]; int n = src_get_api("alGetSource3f", ARG(0), ARG(1), v); if (n == 3) { for (int i = 0; i < 3; i++) if (ARG(2 + i)) wrf32(ARG(2 + i), (float)v[i]); } else if (n) { LK; seterr("alGetSource3f", AL_INVALID_ENUM, ARG(0), ARG(1)); UL; } }
static void al_GetSourcefv(CPU* c) { double v[3]; int n = src_get_api("alGetSourcefv", ARG(0), ARG(1), v); for (int i = 0; i < n && ARG(2); i++) wrf32(ARG(2) + 4 * i, (float)v[i]); }
static void al_GetSourcei(CPU* c) { double v[3]; int n = src_get_api("alGetSourcei", ARG(0), ARG(1), v); if (n == 1 && ARG(2)) wr32(ARG(2), (uint32_t)(int64_t)v[0]); else if (n == 3) { LK; seterr("alGetSourcei", AL_INVALID_ENUM, ARG(0), ARG(1)); UL; } }
static void al_GetSource3i(CPU* c) { double v[3]; int n = src_get_api("alGetSource3i", ARG(0), ARG(1), v); if (n == 3) { for (int i = 0; i < 3; i++) if (ARG(2 + i)) wr32(ARG(2 + i), (uint32_t)(int32_t)v[i]); } else if (n) { LK; seterr("alGetSource3i", AL_INVALID_ENUM, ARG(0), ARG(1)); UL; } }
static void al_GetSourceiv(CPU* c) { double v[3]; int n = src_get_api("alGetSourceiv", ARG(0), ARG(1), v); for (int i = 0; i < n && ARG(2); i++) wr32(ARG(2) + 4 * i, (uint32_t)(int64_t)v[i]); }
typedef void (*SrcOp)(Src*);
static void src_ops(const char* fn, int n, uint32_t ids, SrcOp op) {   /* all-or-nothing */
    LK;
    for (int i = 0; i < n; i++) if (!src_get(rd32(ids + 4 * i))) { seterr(fn, AL_INVALID_NAME, rd32(ids + 4 * i), 0); UL; return; }
    for (int i = 0; i < n; i++) op(src_get(rd32(ids + 4 * i)));
    UL;
}
static void src_pause(Src* s) { if (s->state == AL_PLAYING) s->state = AL_PAUSED; }
static void al_SourcePlay(CPU* c) { LK; Src* s = src_get(ARG(0)); if (s) src_play(s); else seterr("alSourcePlay", AL_INVALID_NAME, ARG(0), 0); UL; }
static void al_SourceStop(CPU* c) { LK; Src* s = src_get(ARG(0)); if (s) src_stop(s); else seterr("alSourceStop", AL_INVALID_NAME, ARG(0), 0); UL; }
static void al_SourceRewind(CPU* c) { LK; Src* s = src_get(ARG(0)); if (s) src_rewind(s); else seterr("alSourceRewind", AL_INVALID_NAME, ARG(0), 0); UL; }
static void al_SourcePause(CPU* c) { LK; Src* s = src_get(ARG(0)); if (s) src_pause(s); else seterr("alSourcePause", AL_INVALID_NAME, ARG(0), 0); UL; }
static void al_SourcePlayv(CPU* c) { src_ops("alSourcePlayv", (int)ARG(0), ARG(1), src_play); }
static void al_SourceStopv(CPU* c) { src_ops("alSourceStopv", (int)ARG(0), ARG(1), src_stop); }
static void al_SourceRewindv(CPU* c) { src_ops("alSourceRewindv", (int)ARG(0), ARG(1), src_rewind); }
static void al_SourcePausev(CPU* c) { src_ops("alSourcePausev", (int)ARG(0), ARG(1), src_pause); }
static void al_SourceQueueBuffers(CPU* c) {
    uint32_t id = ARG(0), ids = ARG(2); int n = (int)ARG(1);
    if (n <= 0) return;
    LK; Src* s = src_get(id);
    if (!s) { seterr("alSourceQueueBuffers", AL_INVALID_NAME, id, 0); UL; return; }
    if (s->type == AL_STATIC) { seterr("alSourceQueueBuffers", AL_INVALID_OPERATION, id, 0); UL; return; }
    for (int i = 0; i < n; i++) { uint32_t b = rd32(ids + 4 * i); if (b && !buf_get(b)) { seterr("alSourceQueueBuffers", AL_INVALID_NAME, id, b); UL; return; } }
    for (int i = 0; i < n; i++) src_push(s, rd32(ids + 4 * i));
    s->type = AL_STREAMING;
    UL;
}
static void al_SourceUnqueueBuffers(CPU* c) {
    uint32_t id = ARG(0), ids = ARG(2); int n = (int)ARG(1);
    if (n <= 0) return;
    LK; Src* s = src_get(id);
    if (!s) { seterr("alSourceUnqueueBuffers", AL_INVALID_NAME, id, 0); UL; return; }
    if (s->loop || s->type != AL_STREAMING || n > src_processed(s)) { seterr("alSourceUnqueueBuffers", AL_INVALID_VALUE, id, (uint32_t)n); UL; return; }
    for (int i = 0; i < n; i++) { wr32(ids + 4 * i, s->q[i]); Buf* b = buf_get(s->q[i]); if (b && b->refs) b->refs--; }
    memmove(s->q, s->q + n, sizeof *s->q * (size_t)(s->nq - n)); s->nq -= n; s->cur -= n; if (s->cur < 0) s->cur = 0;
    if (!s->nq) s->type = AL_UNDETERMINED;
    UL;
}

/* ================= AL: buffers ================= */
static void al_GenBuffers(CPU* c) {
    int n = (int)ARG(0); uint32_t out = ARG(1);
    if (n <= 0) { if (n < 0) { LK; seterr("alGenBuffers", AL_INVALID_VALUE, (uint32_t)n, 0); UL; } return; }
    LK;
    for (int i = 0; i < n; i++) {
        uint32_t k = UINT32_MAX;
        for (uint32_t j = 0; j < g_nbuf; j++) { uint32_t t = (g_bufnext + j) % g_nbuf; if (!g_buf[t].used) { k = t; break; } }
        if (k == UINT32_MAX) {
            if (g_nbuf >= MAXBUF) { seterr("alGenBuffers", AL_OUT_OF_MEMORY, (uint32_t)n, 0); break; }
            if (g_nbuf == g_capbuf) { g_capbuf = g_capbuf ? 2 * g_capbuf : 1024; g_buf = realloc(g_buf, sizeof *g_buf * g_capbuf); }
            k = g_nbuf++;
        }
        memset(&g_buf[k], 0, sizeof g_buf[k]); g_buf[k].used = 1; g_buf[k].ch = 1; g_buf[k].srcch = 1; g_buf[k].bits = 16;
        g_bufnext = k + 1; wr32(out + 4 * i, k + 1); st_genbuf++;
    }
    UL;
}
static void al_DeleteBuffers(CPU* c) {
    int n = (int)ARG(0); uint32_t ids = ARG(1);
    LK;
    for (int i = 0; i < n; i++) {
        uint32_t id = rd32(ids + 4 * i); if (!id) continue;
        Buf* b = buf_get(id);
        if (!b) { seterr("alDeleteBuffers", AL_INVALID_NAME, id, 0); UL; return; }
        if (b->refs) { seterr("alDeleteBuffers", AL_INVALID_OPERATION, id, (uint32_t)b->refs); UL; return; }
    }
    int16_t* fr[64]; int nf = 0;
    for (int i = 0; i < n; i++) {
        Buf* b = buf_get(rd32(ids + 4 * i)); if (!b) continue;
        st_bytes -= (uint64_t)b->frames * b->ch * 2;
        if (nf < 64) fr[nf++] = b->d; else free(b->d);
        b->d = NULL; b->used = 0;
    }
    UL;
    for (int i = 0; i < nf; i++) free(fr[i]);
}
static void al_IsBuffer(CPU* c) { LK; int r = !ARG(0) || buf_get(ARG(0)); UL; RET(r); }
static void al_BufferData(CPU* c) {
    uint32_t id = ARG(0), f = ARG(1), data = ARG(2), size = ARG(3), freq = ARG(4);
    LK; int ok = buf_get(id) != NULL; if (!ok) seterr("alBufferData", AL_INVALID_NAME, id, f); else if (!freq || (int32_t)size < 0) { seterr("alBufferData", AL_INVALID_VALUE, id, freq); ok = 0; }
    UL; if (!ok) return;
    int16_t* d = NULL; uint32_t frames = 0, bytes = 0; int ch = 1, bits = 16, srcch = 1;
    int e = convert(f, data ? M + data : NULL, size, &d, &frames, &ch, &bits, &srcch, &bytes);
    if (e) { LK; seterr("alBufferData", e, id, f); UL; return; }
    LK;
    Buf* b = buf_get(id); int16_t* old = NULL;
    if (!b) { seterr("alBufferData", AL_INVALID_NAME, id, f); old = d; }
    else {
        if (b->refs && st_inuse++ < 5 && g_log) fprintf(stderr, "[al] alBufferData(%u): buffer is queued on a source (replaced anyway)\n", id);
        st_bytes += (uint64_t)frames * ch * 2; st_bytes -= (uint64_t)b->frames * b->ch * 2; st_bufdata++;
        old = b->d; b->d = d; b->frames = frames; b->freq = freq; b->ch = ch; b->bits = bits; b->srcch = srcch; b->bytes = bytes;
    }
    UL;
    free(old);
    if (g_log > 1) fprintf(stderr, "[al] alBufferData(%u, %#x, %u bytes, %u Hz) -> %u frames\n", id, f, size, freq, frames);
}
static int buf_param(const Buf* b, uint32_t p, int* v) {
    switch (p) {
    case AL_FREQUENCY: *v = (int)b->freq; return 1;
    case AL_BITS: *v = b->bits; return 1;
    case AL_CHANNELS: *v = b->srcch; return 1;
    case AL_SIZE: *v = (int)b->bytes; return 1;
    }
    return 0;
}
static void buf_get_api(CPU* c, const char* fn, int ints, int three) {
    uint32_t id = ARG(0), p = ARG(1); int v = 0, ok;
    LK; Buf* b = buf_get(id);
    if (!b) { seterr(fn, AL_INVALID_NAME, id, p); UL; return; }
    ok = !three && buf_param(b, p, &v);
    if (!ok) { seterr(fn, AL_INVALID_ENUM, id, p); UL; return; }
    UL;
    if (ARG(2)) { if (ints) wr32(ARG(2), (uint32_t)v); else wrf32(ARG(2), (float)v); }
}
static void al_GetBufferi(CPU* c) { buf_get_api(c, "alGetBufferi", 1, 0); }
static void al_GetBufferiv(CPU* c) { buf_get_api(c, "alGetBufferiv", 1, 0); }
static void al_GetBufferf(CPU* c) { buf_get_api(c, "alGetBufferf", 0, 0); }
static void al_GetBufferfv(CPU* c) { buf_get_api(c, "alGetBufferfv", 0, 0); }
static void al_GetBuffer3i(CPU* c) { buf_get_api(c, "alGetBuffer3i", 1, 1); }
static void al_GetBuffer3f(CPU* c) { buf_get_api(c, "alGetBuffer3f", 0, 1); }
static void buf_set_api(CPU* c, const char* fn) {   /* OpenAL 1.1 has no settable buffer parameters */
    LK; if (!buf_get(ARG(0))) seterr(fn, AL_INVALID_NAME, ARG(0), ARG(1)); else seterr(fn, AL_INVALID_ENUM, ARG(0), ARG(1)); UL;
}
static void al_Bufferf(CPU* c) { buf_set_api(c, "alBufferf"); }
static void al_Buffer3f(CPU* c) { buf_set_api(c, "alBuffer3f"); }
static void al_Bufferfv(CPU* c) { buf_set_api(c, "alBufferfv"); }
static void al_Bufferi(CPU* c) { buf_set_api(c, "alBufferi"); }
static void al_Buffer3i(CPU* c) { buf_set_api(c, "alBuffer3i"); }
static void al_Bufferiv(CPU* c) { buf_set_api(c, "alBufferiv"); }

/* ================= DirectSound 8 (dsound.dll), v0.9.5 =================
 * Bink (binkw32.dll) plays movie audio through an IDirectSound8 the game creates itself (DirectSoundCreate8 ->
 * SetCooperativeLevel(DSSCL_PRIORITY) -> BinkSetSoundSystem(BinkOpenDirectSound, ds)); the hardware survey calls
 * DirectSoundCreate + GetSpeakerConfig. Software implementation on the mixer above: buffer bytes live in guest memory
 * (Lock returns pointers into them), playing secondary buffers are mixed by hle_al_render with volume, pan and frequency
 * (linear interpolation); the play cursor advances with the mixed frames, the write cursor runs DS_LEAD_MS ahead of it.
 * The primary buffer only stores its format. No 3D buffers, effects or position notifications.
 * ME1_DSOUND=0: DirectSoundCreate* fail with DSERR_NODRIVER as before v0.9.5 (movies silent); ME1_DSOUND_LOG=1 logs calls. */
#define DS_OK_ 0u
#define DSERR_NODRIVER_ 0x88780078u
#define DSERR_INVALIDPARAM_ 0x80070057u
#define DSERR_NOINTERFACE_ 0x80004002u
#define DSERR_BADFORMAT_ 0x88780064u
#define DSERR_OUTOFMEMORY_ 0x8007000Eu
#define DSERR_INVALIDCALL_ 0x88780032u
#define DSERR_UNSUPPORTED_ 0x80004001u
#define DSERR_PRIOLEVELNEEDED_ 0x88780046u
#define DSERR_ALREADYINITIALIZED_ 0x88780082u
#define DS_MAGIC 0x38534448u    /* "HDS8" */
#define DSB_MAGIC 0x42534448u   /* "HDSB" */
#define DSB_MAX 32
#define DS_LEAD_MS 30
typedef struct {
    int used, refs, primary, playing, loop, rampok, shared;
    uint32_t obj, data, bytes, flags;
    int ch, bits, isf, align, freq0, freq, vol, pan;
    uint64_t fp;   /* 32.32 frame position */
    float gl, gr;
} DSB;
static DSB g_dsb[DSB_MAX];
static uint32_t g_ds_vt, g_dsb_vt, g_ds_pfmt[5];   /* primary format: tag/ch, rate, avg, align/bits, cbSize */
static int g_ds_on = -1, g_ds_log;
static unsigned st_ds_create, st_ds_bufs, st_ds_locks;
static uint64_t st_ds_frames;
#define DSL(...) do { if (g_ds_log) fprintf(stderr, "[dsound] " __VA_ARGS__); } while (0)

static float ds_amp(int mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }
static void ds_gains(const DSB* b, float* l, float* r) {
    float g = ds_amp(b->vol), gl = g, gr = g;
    if (b->pan > 0) gl *= ds_amp(-b->pan); else if (b->pan < 0) gr *= ds_amp(b->pan);
    *l = gl; *r = gr;
}
static inline void ds_sample(const DSB* b, const uint8_t* d, uint32_t idx, float* l, float* r) {
    const uint8_t* p = d + (size_t)idx * (uint32_t)b->align;
    float v0, v1;
    if (b->isf) { float f0, f1; memcpy(&f0, p, 4); f1 = f0; if (b->ch > 1) memcpy(&f1, p + 4, 4); v0 = f0 * 32767.0f; v1 = f1 * 32767.0f; }
    else if (b->bits == 16) { int16_t s0, s1; memcpy(&s0, p, 2); s1 = s0; if (b->ch > 1) memcpy(&s1, p + 2, 2); v0 = s0; v1 = s1; }
    else { v0 = (float)((int)p[0] - 128) * 256.0f; v1 = b->ch > 1 ? (float)((int)p[1] - 128) * 256.0f : v0; }
    *l = v0; *r = v1;
}
static void ds_mix(float* mix, int n) {   /* caller holds LK */
    for (int k = 0; k < DSB_MAX; k++) {
        DSB* b = &g_dsb[k];
        if (!b->used || b->primary || !b->playing || !b->align) continue;
        uint32_t nfr = b->bytes / (uint32_t)b->align; if (!nfr) continue;
        float tl, tr; ds_gains(b, &tl, &tr);
        if (!b->rampok) { b->gl = tl; b->gr = tr; b->rampok = 1; }
        float gl = b->gl, gr = b->gr, dl = (tl - gl) / n, dr = (tr - gr) / n;
        int f = b->freq ? b->freq : b->freq0;
        uint64_t st = (uint64_t)((double)f / g_rate * 4294967296.0); if (!st) st = 1;
        const uint8_t* d = M + b->data;
        int i = 0;
        for (; i < n; i++) {
            uint32_t idx = (uint32_t)(b->fp >> 32);
            if (idx >= nfr) {
                if (!b->loop) { b->playing = 0; b->fp = 0; break; }
                b->fp -= (uint64_t)nfr << 32; idx -= nfr; if (idx >= nfr) { b->fp = 0; idx = 0; }
            }
            uint32_t nx = idx + 1 < nfr ? idx + 1 : b->loop ? 0 : idx;
            float t = (float)(uint32_t)b->fp * (1.0f / 4294967296.0f), l0, r0, l1, r1;
            ds_sample(b, d, idx, &l0, &r0); ds_sample(b, d, nx, &l1, &r1);
            mix[2 * i] += (l0 + (l1 - l0) * t) * gl; mix[2 * i + 1] += (r0 + (r1 - r0) * t) * gr;
            gl += dl; gr += dr; b->fp += st;
        }
        st_ds_frames += (uint64_t)i;
        b->gl = tl; b->gr = tr;
    }
}
static DSB* dsb_of(uint32_t self) {
    if (!self || rd32(self + 4) != DSB_MAGIC) return NULL;
    uint32_t i = rd32(self + 8);
    return i < DSB_MAX && g_dsb[i].used && g_dsb[i].obj == self ? &g_dsb[i] : NULL;
}
static void ds_build(void) {   /* caller holds LK */
    if (g_ds_vt) return;
    static const char* const ds[] = { "QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer",
        "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize", "VerifyCertification" };
    static const char* const bf[] = { "QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat", "GetVolume",
        "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play", "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan",
        "SetFrequency", "Stop", "Unlock", "Restore", "SetFX", "AcquireResources", "GetObjectInPath" };
    char nm[96];
    uint32_t vt = ghalloc(4 * 12, 1);
    for (int i = 0; i < 12; i++) { snprintf(nm, sizeof nm, "IDirectSound8::%s", ds[i]); wr32(vt + 4 * i, hle_resolve("dsound.com", nm)); }
    uint32_t vb = ghalloc(4 * 24, 1);
    for (int i = 0; i < 24; i++) { snprintf(nm, sizeof nm, "IDirectSoundBuffer8::%s", bf[i]); wr32(vb + 4 * i, hle_resolve("dsound.com", nm)); }
    g_dsb_vt = vb; g_ds_vt = vt;
    g_ds_pfmt[0] = 1 | 2u << 16; g_ds_pfmt[1] = 22050; g_ds_pfmt[2] = 22050 * 2; g_ds_pfmt[3] = 2 | 8u << 16; g_ds_pfmt[4] = 0;
}
static void ds_Create(CPU* c) {   /* DirectSoundCreate(8)(lpcGuidDevice, ppDS, pUnkOuter) */
    al_init();
    if (g_ds_on < 0) { const char* e = getenv("ME1_DSOUND"); g_ds_on = !(e && !strcmp(e, "0")); e = getenv("ME1_DSOUND_LOG"); g_ds_log = e && atoi(e); }
    if (ARG(1)) wr32(ARG(1), 0);
    if (!g_ds_on || !g_on) { fprintf(stderr, "[dsound] DirectSoundCreate -> DSERR_NODRIVER (%s)\n", g_ds_on ? "ME1_AL=0" : "ME1_DSOUND=0"); RET(DSERR_NODRIVER_); return; }
    if (!ARG(1)) { RET(DSERR_INVALIDPARAM_); return; }
    if (ARG(2)) { RET(0x80040110u /* CLASS_E_NOAGGREGATION */); return; }
    LK; ds_build(); uint32_t o = ghalloc(16, 1); wr32(o, g_ds_vt); wr32(o + 4, DS_MAGIC); wr32(o + 8, 1); st_ds_create++; UL;
    out_start();
    wr32(ARG(1), o);
    fprintf(stderr, "[dsound] DirectSoundCreate -> %08x (software buffers on the %s mixer, %d Hz)\n", o, g_backend, g_rate);
    RET(DS_OK_);
}
/* IUnknown: QueryInterface by the first dword of the IID */
static int iid_in(uint32_t riid, const uint32_t* ok, int n) { if (!riid) return 0; uint32_t d = rd32(riid); for (int i = 0; i < n; i++) if (ok[i] == d) return 1; return 0; }
static void ds_QueryInterface(CPU* c) {
    static const uint32_t ok[] = { 0x00000000u /* IUnknown */, 0x279AFA83u /* IDirectSound */, 0xC50A7E93u /* IDirectSound8 */ };
    if (!ARG(2)) { RET(DSERR_INVALIDPARAM_); return; }
    if (iid_in(ARG(1), ok, 3)) { LK; wr32(ARG(0) + 8, rd32(ARG(0) + 8) + 1); UL; wr32(ARG(2), ARG(0)); RET(DS_OK_); return; }
    wr32(ARG(2), 0); RET(DSERR_NOINTERFACE_);
}
static void ds_AddRef(CPU* c) { LK; uint32_t r = rd32(ARG(0) + 8) + 1; wr32(ARG(0) + 8, r); UL; RET(r); }
static void ds_Release(CPU* c) {
    LK; uint32_t r = rd32(ARG(0) + 8); if (r) r--; wr32(ARG(0) + 8, r);
    if (!r) for (int k = 0; k < DSB_MAX; k++) if (g_dsb[k].used && g_dsb[k].primary) g_dsb[k].playing = 0;
    UL; RET(r);
}
static int fmt_parse(uint32_t w, DSB* b) {
    if (!w) return 0;
    uint32_t tag = rd16(w), ch = rd16(w + 2), rate = rd32(w + 4), bits = rd16(w + 14);
    int isf = tag == 3;
    if (tag == 0xFFFEu && rd16(w + 16) >= 22) { uint32_t sub = rd32(w + 24); if (sub == 3) isf = 1; else if (sub != 1) return 0; }
    else if (tag != 1 && tag != 3) return 0;
    if (ch < 1 || ch > 8 || rate < 100 || rate > 200000) return 0;
    if (isf ? bits != 32 : bits != 8 && bits != 16) return 0;
    b->ch = (int)ch; b->bits = (int)bits; b->isf = isf; b->align = (int)(ch * bits / 8); b->freq0 = (int)rate;
    return 1;
}
static uint32_t dsb_new(DSB** out) {   /* caller holds LK */
    for (int k = 0; k < DSB_MAX; k++) if (!g_dsb[k].used) {
        DSB* b = &g_dsb[k]; memset(b, 0, sizeof *b); b->used = 1; b->refs = 1;
        b->obj = ghalloc(16, 1); wr32(b->obj, g_dsb_vt); wr32(b->obj + 4, DSB_MAGIC); wr32(b->obj + 8, (uint32_t)k);
        *out = b; st_ds_bufs++; return b->obj;
    }
    return 0;
}
static void ds_CreateSoundBuffer(CPU* c) {   /* (this, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, pUnkOuter) */
    uint32_t d = ARG(1), pp = ARG(2);
    if (!d || !pp || (rd32(d) != 36 && rd32(d) != 20)) { RET(DSERR_INVALIDPARAM_); return; }
    wr32(pp, 0);
    uint32_t fl = rd32(d + 4), bytes = rd32(d + 8), wfx = rd32(d + 16);
    LK; DSB* b = NULL; uint32_t o = 0, err = DS_OK_;
    if (fl & 1u /* DSBCAPS_PRIMARYBUFFER */) {
        if (bytes || wfx) err = DSERR_INVALIDPARAM_;
        else if ((o = dsb_new(&b))) { b->primary = 1; b->flags = fl; }
        else err = DSERR_OUTOFMEMORY_;
    } else {
        DSB t; memset(&t, 0, sizeof t);
        if (!fmt_parse(wfx, &t)) err = DSERR_BADFORMAT_;
        else if (bytes < 4 || bytes > 0x0FFFFFFFu) err = DSERR_INVALIDPARAM_;
        else if (!(o = dsb_new(&b))) err = DSERR_OUTOFMEMORY_;
        else {
            uint32_t ob = b->obj; *b = t; b->used = 1; b->refs = 1; b->obj = ob; b->flags = fl;
            b->bytes = bytes - bytes % (uint32_t)t.align;
            b->data = ghalloc(bytes, 1);
            if (!b->data) { b->used = 0; o = 0; err = DSERR_OUTOFMEMORY_; }
            else if (t.bits == 8) memset(M + b->data, 0x80, bytes);
        }
    }
    UL;
    if (o) wr32(pp, o);
    DSL("CreateSoundBuffer(flags %x, %u bytes, %s %d ch %d bit %d Hz) -> %08x err %08x\n", fl, bytes, b && b->primary ? "primary" : "secondary",
        b ? b->ch : 0, b ? b->bits : 0, b ? b->freq0 : 0, o, err);
    RET(err);
}
static void ds_GetCaps(CPU* c) {
    uint32_t p = ARG(1); if (!p || rd32(p) != 96) { RET(DSERR_INVALIDPARAM_); return; }
    memset(M + p + 4, 0, 92);
    wr32(p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800);   /* PRIMARY/SECONDARY MONO|STEREO|8|16 bit, CONTINUOUSRATE */
    wr32(p + 8, 100); wr32(p + 12, 200000); wr32(p + 16, 1);
    RET(DS_OK_);
}
static void ds_DuplicateSoundBuffer(CPU* c) {   /* (this, original, *dup) */
    uint32_t pp = ARG(2); if (!pp) { RET(DSERR_INVALIDPARAM_); return; }
    wr32(pp, 0);
    LK; DSB* s = dsb_of(ARG(1)); DSB* b = NULL; uint32_t o = 0, err = DS_OK_;
    if (!s || s->primary) err = DSERR_INVALIDCALL_;
    else if (!(o = dsb_new(&b))) err = DSERR_OUTOFMEMORY_;
    else { uint32_t ob = b->obj; *b = *s; b->obj = ob; b->refs = 1; b->playing = 0; b->fp = 0; b->rampok = 0; b->shared = 1; s->shared = 1; }
    UL;
    if (o) wr32(pp, o);
    RET(err);
}
static void ds_GetSpeakerConfig(CPU* c) { if (!ARG(1)) { RET(DSERR_INVALIDPARAM_); return; } wr32(ARG(1), 4 /* DSSPEAKER_STEREO */ | 20u << 16 /* GEOMETRY_WIDE */); RET(DS_OK_); }
static void ds_Initialize(CPU* c) { RET(DSERR_ALREADYINITIALIZED_); }
static void ds_VerifyCertification(CPU* c) { if (ARG(1)) wr32(ARG(1), 0 /* DS_CERTIFIED */); RET(DS_OK_); }
static void ds_ok(CPU* c) { RET(DS_OK_); }

static void b_QueryInterface(CPU* c) {
    static const uint32_t ok[] = { 0x00000000u /* IUnknown */, 0x279AFA85u /* IDirectSoundBuffer */, 0x6825A449u /* IDirectSoundBuffer8 */ };
    if (!ARG(2)) { RET(DSERR_INVALIDPARAM_); return; }
    LK; DSB* b = dsb_of(ARG(0)); int y = b && iid_in(ARG(1), ok, b->primary ? 2 : 3); if (y) b->refs++; UL;
    DSL("Buffer QueryInterface(%08x) -> %s\n", ARG(1) ? rd32(ARG(1)) : 0, y ? "ok" : "E_NOINTERFACE");
    wr32(ARG(2), y ? ARG(0) : 0); RET(y ? DS_OK_ : DSERR_NOINTERFACE_);
}
static void b_AddRef(CPU* c) { LK; DSB* b = dsb_of(ARG(0)); uint32_t r = b ? (uint32_t)++b->refs : 0; UL; RET(r); }
static void b_Release(CPU* c) {
    LK; DSB* b = dsb_of(ARG(0)); uint32_t r = 0, freedata = 0;
    if (b && --b->refs <= 0) {
        int other = 0;
        if (b->shared) for (int k = 0; k < DSB_MAX; k++) if (&g_dsb[k] != b && g_dsb[k].used && g_dsb[k].data == b->data) other = 1;
        if (!other && b->data) freedata = b->data;
        wr32(b->obj + 4, 0); b->used = 0; b->playing = 0;
    } else if (b) r = (uint32_t)b->refs;
    UL;
    if (freedata) ghfree(freedata);
    RET(r);
}
#define BUF(b, err) DSB* b; LK; b = dsb_of(ARG(0)); if (!b) { UL; RET(err); return; }
static void b_GetCaps(CPU* c) {
    uint32_t p = ARG(1); if (!p || rd32(p) != 20) { RET(DSERR_INVALIDPARAM_); return; }
    BUF(b, DSERR_INVALIDCALL_)
    wr32(p + 4, (b->flags & ~0x4u) | 0x8u /* LOCSOFTWARE */); wr32(p + 8, b->primary ? 0 : b->bytes); wr32(p + 12, 0); wr32(p + 16, 0);
    UL; RET(DS_OK_);
}
static uint32_t lead_bytes(const DSB* b) {
    uint32_t f = (uint32_t)(b->freq ? b->freq : b->freq0), l = (uint32_t)((uint64_t)f * DS_LEAD_MS / 1000) * (uint32_t)b->align;
    return l < b->bytes / 2 ? l : b->bytes / 2 - (b->bytes / 2) % (uint32_t)b->align;
}
static void b_GetCurrentPosition(CPU* c) {   /* (this, pdwPlay, pdwWrite) */
    BUF(b, DSERR_INVALIDCALL_)
    uint32_t play = 0, wr = 0;
    if (!b->primary && b->bytes) {
        play = (uint32_t)(b->fp >> 32) * (uint32_t)b->align; if (play >= b->bytes) play = 0;
        wr = b->playing ? (play + lead_bytes(b)) % b->bytes : play;
    }
    UL;
    if (ARG(1)) wr32(ARG(1), play);
    if (ARG(2)) wr32(ARG(2), wr);
    RET(DS_OK_);
}
static void b_GetFormat(CPU* c) {   /* (this, pwfx, sizeAllocated, pSizeWritten) */
    BUF(b, DSERR_INVALIDCALL_)
    uint8_t w[18]; memset(w, 0, sizeof w);
    if (b->primary) { memcpy(w, g_ds_pfmt, 16); }
    else {
        uint16_t tag = b->isf ? 3 : 1, ch = (uint16_t)b->ch, al = (uint16_t)b->align, bits = (uint16_t)b->bits; uint32_t r = (uint32_t)b->freq0, avg = r * (uint32_t)b->align;
        memcpy(w, &tag, 2); memcpy(w + 2, &ch, 2); memcpy(w + 4, &r, 4); memcpy(w + 8, &avg, 4); memcpy(w + 12, &al, 2); memcpy(w + 14, &bits, 2);
    }
    UL;
    uint32_t n = ARG(2) < 18 ? ARG(2) : 18;
    if (ARG(1) && n) memcpy(M + ARG(1), w, n);
    if (ARG(3)) wr32(ARG(3), ARG(1) ? n : 18);
    RET(ARG(1) || ARG(3) ? DS_OK_ : DSERR_INVALIDPARAM_);
}
static void b_GetVolume(CPU* c) { BUF(b, DSERR_INVALIDCALL_) int v = b->vol; UL; if (!ARG(1)) { RET(DSERR_INVALIDPARAM_); return; } wr32(ARG(1), (uint32_t)v); RET(DS_OK_); }
static void b_GetPan(CPU* c) { BUF(b, DSERR_INVALIDCALL_) int v = b->pan; UL; if (!ARG(1)) { RET(DSERR_INVALIDPARAM_); return; } wr32(ARG(1), (uint32_t)v); RET(DS_OK_); }
static void b_GetFrequency(CPU* c) { BUF(b, DSERR_INVALIDCALL_) int v = b->freq ? b->freq : b->freq0; UL; if (!ARG(1)) { RET(DSERR_INVALIDPARAM_); return; } wr32(ARG(1), (uint32_t)v); RET(DS_OK_); }
static void b_GetStatus(CPU* c) {
    BUF(b, DSERR_INVALIDCALL_) uint32_t s = b->playing ? 1u | (b->loop ? 4u : 0u) : 0u; UL;
    if (!ARG(1)) { RET(DSERR_INVALIDPARAM_); return; }
    wr32(ARG(1), s); RET(DS_OK_);
}
static void b_Initialize(CPU* c) { RET(DSERR_ALREADYINITIALIZED_); }
static void b_Lock(CPU* c) {   /* (this, offset, bytes, ppv1, pn1, ppv2, pn2, flags) */
    BUF(b, DSERR_INVALIDCALL_)
    if (b->primary) { UL; RET(DSERR_PRIOLEVELNEEDED_); return; }
    uint32_t off = ARG(1), n = ARG(2), fl = ARG(7);
    if (fl & 1u /* DSBLOCK_FROMWRITECURSOR */) {
        uint32_t play = (uint32_t)(b->fp >> 32) * (uint32_t)b->align; if (play >= b->bytes) play = 0;
        off = b->playing ? (play + lead_bytes(b)) % b->bytes : play;
    }
    if (fl & 2u /* DSBLOCK_ENTIREBUFFER */) n = b->bytes;
    uint32_t data = b->data, size = b->bytes;
    st_ds_locks++;
    UL;
    if (off >= size || !n || n > size || !ARG(3) || !ARG(4)) { RET(DSERR_INVALIDPARAM_); return; }
    uint32_t n1 = n < size - off ? n : size - off, n2 = n - n1;
    wr32(ARG(3), data + off); wr32(ARG(4), n1);
    if (ARG(5)) wr32(ARG(5), n2 ? data : 0);
    if (ARG(6)) wr32(ARG(6), n2);
    RET(DS_OK_);
}
static void b_Play(CPU* c) {   /* (this, reserved, priority, flags) */
    BUF(b, DSERR_INVALIDCALL_)
    if (!b->playing) b->rampok = 0;
    b->playing = 1; b->loop = (ARG(3) & 1u) != 0;
    int pr = b->primary;
    UL;
    DSL("Play(%08x, %s)%s\n", ARG(0), ARG(3) & 1u ? "looping" : "once", pr ? " primary" : "");
    RET(DS_OK_);
}
static void b_SetCurrentPosition(CPU* c) {
    BUF(b, DSERR_INVALIDCALL_)
    uint32_t r = DS_OK_;
    if (b->primary) r = DSERR_INVALIDCALL_;
    else if (ARG(1) >= b->bytes) r = DSERR_INVALIDPARAM_;
    else b->fp = (uint64_t)(ARG(1) / (uint32_t)b->align) << 32;
    UL; RET(r);
}
static void b_SetFormat(CPU* c) {
    BUF(b, DSERR_INVALIDCALL_)
    uint32_t r = DS_OK_; DSB t;
    if (!b->primary) r = DSERR_INVALIDCALL_;
    else if (!fmt_parse(ARG(1), &t)) r = DSERR_BADFORMAT_;
    else memcpy(g_ds_pfmt, M + ARG(1), 16);
    UL; RET(r);
}
static void b_SetVolume(CPU* c) {
    int32_t v = (int32_t)ARG(1); if (v < -10000 || v > 0) { RET(DSERR_INVALIDPARAM_); return; }
    BUF(b, DSERR_INVALIDCALL_) b->vol = v; UL; RET(DS_OK_);
}
static void b_SetPan(CPU* c) {
    int32_t v = (int32_t)ARG(1); if (v < -10000 || v > 10000) { RET(DSERR_INVALIDPARAM_); return; }
    BUF(b, DSERR_INVALIDCALL_) b->pan = v; UL; RET(DS_OK_);
}
static void b_SetFrequency(CPU* c) {
    uint32_t f = ARG(1); if (f && (f < 100 || f > 200000)) { RET(DSERR_INVALIDPARAM_); return; }
    BUF(b, DSERR_INVALIDCALL_) uint32_t r = b->primary ? DSERR_INVALIDCALL_ : DS_OK_; if (!b->primary) b->freq = (int)f; UL; RET(r);
}
static void b_Stop(CPU* c) { BUF(b, DSERR_INVALIDCALL_) b->playing = 0; UL; RET(DS_OK_); }
static void b_Unlock(CPU* c) { BUF(b, DSERR_INVALIDCALL_) UL; RET(DS_OK_); }
static void b_unsup(CPU* c) { RET(DSERR_UNSUPPORTED_); }
static void b_ok(CPU* c) { RET(DS_OK_); }
static void b_noint(CPU* c) { if (ARG(4)) wr32(ARG(4), 0); RET(DSERR_NOINTERFACE_); }
static void ds_brief(char* o, size_t n) {
    int nb = 0, pl = 0;
    for (int k = 0; k < DSB_MAX; k++) if (g_dsb[k].used && !g_dsb[k].primary) { nb++; pl += g_dsb[k].playing; }
    snprintf(o, n, "; dsound: %d/%d buf playing, %.1f s mixed", pl, nb, g_rate ? (double)st_ds_frames / g_rate : 0.0);
}

/* ---- status ---- */
void hle_al_brief(char* o, size_t n) {
    if (!g_ndev && !st_gensrc && !st_ds_create) { snprintf(o, n, "al: no device"); return; }
    LK; int act = 0, used = 0, nb = 0;
    for (int k = 0; k < MAXSRC; k++) if (g_src[k].used) { used++; act += g_src[k].state == AL_PLAYING; }
    for (uint32_t k = 0; k < g_nbuf; k++) nb += g_buf[k].used;
    int w = snprintf(o, n, "al: %d/%d src playing, %d buf %llu MB, %llu plays, %s %d Hz", act, used, nb, (unsigned long long)(st_bytes >> 20),
             (unsigned long long)st_plays, g_backend, g_rate);
    if (st_ds_create && w > 0 && (size_t)w < n) ds_brief(o + w, n - (size_t)w);
    UL;
}
void hle_al_report(void) {
    if (!g_ndev && !st_gensrc && !st_genbuf && !st_ds_create) return;
    char b[256]; hle_al_brief(b, sizeof b);
    fprintf(stderr, "[al] report: %s; %u sources and %u buffers generated, %llu alBufferData, %.1f s mixed (max %u voices), %u errors, %u output restarts\n",
            b, st_gensrc, st_genbuf, (unsigned long long)st_bufdata, g_rate ? (double)st_frames / g_rate : 0.0, st_maxact, st_errs, st_restarts);
    if (g_wav) wav_header();
}

#define D(n, f) { "OpenAL32.dll", n, 0, f, HLE_NOLOCK },
const HleDef hle_al_defs[] = {
    D("alcOpenDevice", alc_OpenDevice) D("alcCloseDevice", alc_CloseDevice) D("alcCreateContext", alc_CreateContext)
    D("alcMakeContextCurrent", alc_MakeContextCurrent) D("alcDestroyContext", alc_DestroyContext) D("alcGetCurrentContext", alc_GetCurrentContext)
    D("alcGetContextsDevice", alc_GetContextsDevice) D("alcProcessContext", alc_ProcessContext) D("alcSuspendContext", alc_SuspendContext)
    D("alcGetError", alc_GetError) D("alcIsExtensionPresent", alc_IsExtensionPresent) D("alcGetProcAddress", alc_GetProcAddress)
    D("alcGetEnumValue", alc_GetEnumValue) D("alcGetString", alc_GetString) D("alcGetIntegerv", alc_GetIntegerv)
    D("alcCaptureOpenDevice", alc_CaptureOpenDevice) D("alcCaptureCloseDevice", alc_CaptureCloseDevice) D("alcCaptureStart", alc_Nop)
    D("alcCaptureStop", alc_Nop) D("alcCaptureSamples", alc_Nop)
    D("alGetError", al_GetError) D("alEnable", al_Enable) D("alDisable", al_Disable) D("alIsEnabled", al_IsEnabled)
    D("alGetBoolean", al_GetBoolean) D("alGetInteger", al_GetInteger) D("alGetFloat", al_GetFloat) D("alGetDouble", al_GetDouble)
    D("alGetBooleanv", al_GetBooleanv) D("alGetIntegerv", al_GetIntegerv) D("alGetFloatv", al_GetFloatv) D("alGetDoublev", al_GetDoublev)
    D("alDopplerFactor", al_DopplerFactor) D("alDopplerVelocity", al_DopplerVelocity) D("alSpeedOfSound", al_SpeedOfSound)
    D("alDistanceModel", al_DistanceModel) D("alGetString", al_GetString) D("alIsExtensionPresent", al_IsExtensionPresent)
    D("alGetProcAddress", al_GetProcAddress) D("alGetEnumValue", al_GetEnumValue)
    D("alListenerf", al_Listenerf) D("alListener3f", al_Listener3f) D("alListenerfv", al_Listenerfv) D("alListeneri", al_Listeneri)
    D("alListener3i", al_Listener3i) D("alListeneriv", al_Listeneriv) D("alGetListenerf", al_GetListenerf) D("alGetListener3f", al_GetListener3f)
    D("alGetListenerfv", al_GetListenerfv) D("alGetListeneri", al_GetListeneri) D("alGetListener3i", al_GetListener3i)
    D("alGetListeneriv", al_GetListeneriv)
    D("alGenSources", al_GenSources) D("alDeleteSources", al_DeleteSources) D("alIsSource", al_IsSource) D("alSourcef", al_Sourcef)
    D("alSource3f", al_Source3f) D("alSourcefv", al_Sourcefv) D("alSourcei", al_Sourcei) D("alSource3i", al_Source3i)
    D("alSourceiv", al_Sourceiv) D("alGetSourcef", al_GetSourcef) D("alGetSource3f", al_GetSource3f) D("alGetSourcefv", al_GetSourcefv)
    D("alGetSourcei", al_GetSourcei) D("alGetSource3i", al_GetSource3i) D("alGetSourceiv", al_GetSourceiv)
    D("alSourcePlay", al_SourcePlay) D("alSourceStop", al_SourceStop) D("alSourceRewind", al_SourceRewind) D("alSourcePause", al_SourcePause)
    D("alSourcePlayv", al_SourcePlayv) D("alSourceStopv", al_SourceStopv) D("alSourceRewindv", al_SourceRewindv)
    D("alSourcePausev", al_SourcePausev) D("alSourceQueueBuffers", al_SourceQueueBuffers) D("alSourceUnqueueBuffers", al_SourceUnqueueBuffers)
    D("alGenBuffers", al_GenBuffers) D("alDeleteBuffers", al_DeleteBuffers) D("alIsBuffer", al_IsBuffer) D("alBufferData", al_BufferData)
    D("alBufferf", al_Bufferf) D("alBuffer3f", al_Buffer3f) D("alBufferfv", al_Bufferfv) D("alBufferi", al_Bufferi)
    D("alBuffer3i", al_Buffer3i) D("alBufferiv", al_Bufferiv) D("alGetBufferf", al_GetBufferf) D("alGetBuffer3f", al_GetBuffer3f)
    D("alGetBufferfv", al_GetBufferfv) D("alGetBufferi", al_GetBufferi) D("alGetBuffer3i", al_GetBuffer3i) D("alGetBufferiv", al_GetBufferiv)
#define S(n, a, f) { "dsound.com", n, a, f, HLE_NOLOCK },
    { "dsound.dll", "#1", 3, ds_Create, HLE_NOLOCK }, { "dsound.dll", "#11", 3, ds_Create, HLE_NOLOCK },
    { "dsound.dll", "DirectSoundCreate", 3, ds_Create, HLE_NOLOCK }, { "dsound.dll", "DirectSoundCreate8", 3, ds_Create, HLE_NOLOCK },
    S("IDirectSound8::QueryInterface", 3, ds_QueryInterface) S("IDirectSound8::AddRef", 1, ds_AddRef) S("IDirectSound8::Release", 1, ds_Release)
    S("IDirectSound8::CreateSoundBuffer", 4, ds_CreateSoundBuffer) S("IDirectSound8::GetCaps", 2, ds_GetCaps)
    S("IDirectSound8::DuplicateSoundBuffer", 3, ds_DuplicateSoundBuffer) S("IDirectSound8::SetCooperativeLevel", 3, ds_ok)
    S("IDirectSound8::Compact", 1, ds_ok) S("IDirectSound8::GetSpeakerConfig", 2, ds_GetSpeakerConfig) S("IDirectSound8::SetSpeakerConfig", 2, ds_ok)
    S("IDirectSound8::Initialize", 2, ds_Initialize) S("IDirectSound8::VerifyCertification", 2, ds_VerifyCertification)
    S("IDirectSoundBuffer8::QueryInterface", 3, b_QueryInterface) S("IDirectSoundBuffer8::AddRef", 1, b_AddRef) S("IDirectSoundBuffer8::Release", 1, b_Release)
    S("IDirectSoundBuffer8::GetCaps", 2, b_GetCaps) S("IDirectSoundBuffer8::GetCurrentPosition", 3, b_GetCurrentPosition)
    S("IDirectSoundBuffer8::GetFormat", 4, b_GetFormat) S("IDirectSoundBuffer8::GetVolume", 2, b_GetVolume) S("IDirectSoundBuffer8::GetPan", 2, b_GetPan)
    S("IDirectSoundBuffer8::GetFrequency", 2, b_GetFrequency) S("IDirectSoundBuffer8::GetStatus", 2, b_GetStatus)
    S("IDirectSoundBuffer8::Initialize", 3, b_Initialize) S("IDirectSoundBuffer8::Lock", 8, b_Lock) S("IDirectSoundBuffer8::Play", 4, b_Play)
    S("IDirectSoundBuffer8::SetCurrentPosition", 2, b_SetCurrentPosition) S("IDirectSoundBuffer8::SetFormat", 2, b_SetFormat)
    S("IDirectSoundBuffer8::SetVolume", 2, b_SetVolume) S("IDirectSoundBuffer8::SetPan", 2, b_SetPan) S("IDirectSoundBuffer8::SetFrequency", 2, b_SetFrequency)
    S("IDirectSoundBuffer8::Stop", 1, b_Stop) S("IDirectSoundBuffer8::Unlock", 5, b_Unlock) S("IDirectSoundBuffer8::Restore", 1, b_ok)
    S("IDirectSoundBuffer8::SetFX", 4, b_unsup) S("IDirectSoundBuffer8::AcquireResources", 4, b_ok)
    S("IDirectSoundBuffer8::GetObjectInPath", 5, b_noint)
};
#undef S
#undef D
const unsigned hle_al_ndefs = sizeof hle_al_defs / sizeof hle_al_defs[0];
