/* linux/ui.c - immediate-mode UI for me1-launcher and the me1-linux loading screen (see ui.h). */
#define _GNU_SOURCE
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop

/* embedded fonts (linux/assets.S) */
extern const unsigned char me1a_exo_reg[], me1a_exo_bold[], me1a_mono[], me1a_sym[];

Ui ui;
static SDL_Renderer* R;

/* ------------------------------------------------------------------------------------------ fonts --- */
typedef struct { stbtt_fontinfo fi; int ok; float asc, desc, gap; } Font;
static Font F[4];                                   /* 0 Exo 2, 1 Exo 2 SemiBold, 2 DejaVu Sans Mono, 3 DejaVu symbols */
static const int CHAIN[UF_N][2] = { { 0, 3 }, { 1, 3 }, { 2, 3 } };
static int font_init(Font* f, const unsigned char* d) {
    if (!stbtt_InitFont(&f->fi, d, stbtt_GetFontOffsetForIndex(d, 0))) return 0;
    int a, de, g; stbtt_GetFontVMetrics(&f->fi, &a, &de, &g); f->asc = (float)a; f->desc = (float)de; f->gap = (float)g;
    return f->ok = 1;
}
static float fscale(const Font* f, int px) { return stbtt_ScaleForMappingEmToPixels(&f->fi, (float)px); }

/* glyph cache: (face, px, codepoint) -> atlas rectangle */
typedef struct { uint32_t key; short x, y, w, h, xo, yo; float adv; unsigned char font; int gi; } Glyph;
#define GCAP 8192
#define AW 1024
#define AH 1024
static Glyph G[GCAP]; static int g_ng;
static SDL_Texture* A; static int ax = 1, ay = 1, arow;
static void atlas_reset(void) { memset(G, 0, sizeof G); g_ng = 0; ax = ay = 1; arow = 0; ui.redraw = 1; }
static Glyph* glyph(int face, int px, uint32_t cp) {
    if (px < 4) px = 4; if (px > 127) px = 127;
    uint32_t key = (uint32_t)face << 28 | (uint32_t)px << 21 | (cp & 0x1FFFFF);
    uint32_t h = (key * 2654435761u) >> 19;
    for (;; h++) { Glyph* g = &G[h & (GCAP - 1)]; if (g->key == key) return g; if (!g->key) break; }
    if (g_ng > GCAP * 3 / 4) atlas_reset();
    int fo = -1, gi = 0;
    for (int k = 0; k < 2 && fo < 0; k++) { int f = CHAIN[face][k]; if (F[f].ok && (gi = stbtt_FindGlyphIndex(&F[f].fi, (int)cp))) fo = f; }
    if (fo < 0) { fo = CHAIN[face][0]; gi = 0; }
    float s = fscale(&F[fo], px);
    int x0, y0, x1, y1, adv, lsb;
    stbtt_GetGlyphBitmapBox(&F[fo].fi, gi, s, s, &x0, &y0, &x1, &y1);
    stbtt_GetGlyphHMetrics(&F[fo].fi, gi, &adv, &lsb);
    int w = x1 - x0, hh = y1 - y0, gx = 0, gy = 0;
    if (cp == ' ' || cp == 0xA0) w = hh = 0;
    if (w > 0 && hh > 0) {
        if (w > AW - 2 || hh > AH - 2) w = hh = 0;
        else {
            if (ax + w + 1 > AW) { ax = 1; ay += arow + 1; arow = 0; }
            if (ay + hh + 1 > AH) { atlas_reset(); return glyph(face, px, cp); }
            gx = ax; gy = ay; ax += w + 1; if (hh > arow) arow = hh;
            unsigned char* bm = calloc((size_t)w * hh, 1); uint32_t* p = malloc((size_t)w * hh * 4);
            stbtt_MakeGlyphBitmap(&F[fo].fi, bm, w, hh, w, s, s, gi);
            for (int i = 0; i < w * hh; i++) p[i] = (uint32_t)bm[i] << 24 | 0xFFFFFFu;
            SDL_Rect r = { gx, gy, w, hh }; SDL_UpdateTexture(A, &r, p, w * 4);
            free(bm); free(p);
        }
    }
    h = (key * 2654435761u) >> 19;
    Glyph* g; for (;; h++) { g = &G[h & (GCAP - 1)]; if (!g->key) break; }
    g->key = key; g->x = (short)gx; g->y = (short)gy; g->w = (short)w; g->h = (short)hh; g->xo = (short)x0; g->yo = (short)y0;
    g->adv = adv * s; g->font = (unsigned char)fo; g->gi = gi; g_ng++;
    return g;
}
static uint32_t u8next(const char** pp, const char* end) {
    const unsigned char* p = (const unsigned char*)*pp; uint32_t c = *p++; int n = 0;
    if (c >= 0xF0) { c &= 7; n = 3; } else if (c >= 0xE0) { c &= 15; n = 2; } else if (c >= 0xC0) { c &= 31; n = 1; } else if (c >= 0x80) c = 0xFFFD;
    while (n-- && (const char*)p < end && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 63);
    *pp = (const char*)p; return c;
}
int ui_lineh(int face, int px) { const Font* f = &F[CHAIN[face][0]]; return (int)((f->asc - f->desc + f->gap) * fscale(f, px) + 0.5f); }
static int ascent(int face, int px) { const Font* f = &F[CHAIN[face][0]]; return (int)(f->asc * fscale(f, px) + 0.5f); }
static float run(int face, int px, int x, int y, UiCol c, const char* s, int n, int draw) {
    if (!s) return (float)x;
    const char* e = s + (n < 0 ? (int)strlen(s) : n);
    float fx = (float)x; int base = y + ascent(face, px), prevgi = -1, prevf = -1;
    if (draw) { SDL_SetTextureColorMod(A, c.r, c.g, c.b); SDL_SetTextureAlphaMod(A, c.a); }
    while (s < e) {
        uint32_t cp = u8next(&s, e);
        if (cp == '\n' || cp == '\r') continue;
        if (cp == '\t') { fx += ui_textw(face, px, "    ", 4); prevgi = -1; continue; }
        Glyph* g = glyph(face, px, cp);
        if (prevgi >= 0 && prevf == g->font) fx += stbtt_GetGlyphKernAdvance(&F[g->font].fi, prevgi, g->gi) * fscale(&F[g->font], px);
        if (draw && g->w > 0) {
            SDL_Rect src = { g->x, g->y, g->w, g->h }, dst = { (int)(fx + 0.5f) + g->xo, base + g->yo, g->w, g->h };
            SDL_RenderCopy(R, A, &src, &dst);
        }
        fx += g->adv; prevgi = g->gi; prevf = g->font;
    }
    return fx;
}
int ui_textw(int face, int px, const char* s, int n) { return (int)(run(face, px, 0, 0, UIC(0), s, n, 0) + 0.5f); }
int ui_text(int face, int px, int x, int y, UiCol c, const char* s, int n) { return (int)(run(face, px, x, y, c, s, n, 1) + 0.5f); }
int ui_text_fit(int face, int px, int x, int y, int maxw, UiCol c, const char* s) {
    int n = (int)strlen(s);
    if (ui_textw(face, px, s, n) <= maxw) return ui_text(face, px, x, y, c, s, n);
    int ew = ui_textw(face, px, "…", -1);
    while (n > 0) {
        n--; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        if (ui_textw(face, px, s, n) + ew <= maxw) break;
    }
    int ex = ui_text(face, px, x, y, c, s, n);
    return ui_text(face, px, ex, y, c, "…", -1);
}
int ui_text_c(int face, int px, SDL_Rect r, UiCol c, const char* s) {
    int w = ui_textw(face, px, s, -1), lh = ui_lineh(face, px);
    if (w > r.w - 4) return ui_text_fit(face, px, r.x + 2, r.y + (r.h - lh) / 2, r.w - 4, c, s);
    return ui_text(face, px, r.x + (r.w - w) / 2, r.y + (r.h - lh) / 2, c, s, -1);
}
/* word wrap: breaks at spaces (and after '/' or '-' inside long words), hard breaks at '\n' */
int ui_wrap(int face, int px, const char* s, int maxw, UiLine* out, int max) {
    int nl = 0;
    if (!s) return 0;
    while (*s && nl < max) {
        const char* eol = strchr(s, '\n'); if (!eol) eol = s + strlen(s);
        const char* p = s;
        if (p == eol) { out[nl].s = s; out[nl].n = 0; nl++; s = *eol ? eol + 1 : eol; continue; }
        while (p < eol && nl < max) {
            const char* q = p, *brk = NULL; float w = 0;
            while (q < eol) {
                const char* c0 = q; uint32_t cp = u8next(&q, eol);
                w = (float)ui_textw(face, px, p, (int)(q - p));
                if (w > maxw && c0 > p) { q = c0; break; }
                if (cp == ' ' || cp == '/' || cp == '-' || cp == ',') brk = cp == ' ' ? c0 : q;
            }
            const char* end = q;
            if (q < eol && brk && brk > p) end = brk;
            int n = (int)(end - p); while (n > 0 && p[n - 1] == ' ') n--;
            out[nl].s = p; out[nl].n = n; nl++;
            p = end; while (p < eol && *p == ' ') p++;
        }
        s = *eol ? eol + 1 : eol;
    }
    return nl;
}
#define WRAPMAX 400
int ui_textbox(int face, int px, int x, int y, int maxw, UiCol c, const char* s) {
    static UiLine L[WRAPMAX]; int n = ui_wrap(face, px, s, maxw, L, WRAPMAX), lh = ui_lineh(face, px);
    SDL_Rect clip; SDL_RenderGetClipRect(R, &clip); int clipped = SDL_RenderIsClipEnabled(R);
    for (int i = 0; i < n; i++) {
        int ly = y + i * lh;
        if (clipped && (ly > clip.y + clip.h || ly + lh < clip.y)) continue;
        ui_text(face, px, x, ly, c, L[i].s, L[i].n);
    }
    return n * lh;
}
int ui_textbox_h(int face, int px, int maxw, const char* s) {
    static UiLine L[WRAPMAX]; return ui_wrap(face, px, s, maxw, L, WRAPMAX) * ui_lineh(face, px);
}

/* ---------------------------------------------------------------------------------------- drawing --- */
void ui_fill(SDL_Rect r, UiCol c) { SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a); SDL_RenderFillRect(R, &r); }
#if SDL_VERSION_ATLEAST(2, 0, 18)
#define SEG 6
static int rr_path(SDL_Rect r, float rad, SDL_FPoint* p) {   /* clockwise outline, 4 * (SEG + 1) points */
    float x0 = r.x + rad, y0 = r.y + rad, x1 = r.x + r.w - rad, y1 = r.y + r.h - rad; int n = 0;
    const float cx[4] = { x1, x1, x0, x0 }, cy[4] = { y0, y1, y1, y0 }, a0[4] = { -90, 0, 90, 180 };
    for (int k = 0; k < 4; k++) for (int i = 0; i <= SEG; i++) {
        float a = (a0[k] + 90.0f * i / SEG) * 3.14159265f / 180.0f;
        p[n].x = cx[k] + rad * cosf(a); p[n].y = cy[k] + rad * sinf(a); n++;
    }
    return n;
}
#endif
void ui_rrect(SDL_Rect r, int rad, UiCol c) {
    if (r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.h) rad = r.h / 2; if (rad * 2 > r.w) rad = r.w / 2;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (rad >= 2) {
        SDL_FPoint p[4 * (SEG + 1)]; int n = rr_path(r, (float)rad, p);
        SDL_Vertex v[4 * (SEG + 1) + 1]; int idx[3 * 4 * (SEG + 1)], ni = 0;
        SDL_Color col = { c.r, c.g, c.b, c.a };
        v[0].position.x = r.x + r.w / 2.0f; v[0].position.y = r.y + r.h / 2.0f; v[0].color = col; v[0].tex_coord.x = v[0].tex_coord.y = 0;
        for (int i = 0; i < n; i++) { v[i + 1].position = p[i]; v[i + 1].color = col; v[i + 1].tex_coord.x = v[i + 1].tex_coord.y = 0; }
        for (int i = 0; i < n; i++) { idx[ni++] = 0; idx[ni++] = 1 + i; idx[ni++] = 1 + (i + 1) % n; }
        SDL_RenderGeometry(R, NULL, v, n + 1, idx, ni);
        return;
    }
#endif
    ui_fill(r, c);
}
void ui_rframe(SDL_Rect r, int rad, int t, UiCol c) {
    if (r.w <= 0 || r.h <= 0 || t <= 0) return;
    if (rad * 2 > r.h) rad = r.h / 2; if (rad * 2 > r.w) rad = r.w / 2;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (rad >= 2) {
        SDL_Rect in = { r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t };
        SDL_FPoint po[4 * (SEG + 1)], pi[4 * (SEG + 1)]; int n = rr_path(r, (float)rad, po);
        rr_path(in, (float)(rad - t > 0 ? rad - t : 0.01f), pi);
        SDL_Vertex v[8 * (SEG + 1)]; int idx[6 * 4 * (SEG + 1)], ni = 0; SDL_Color col = { c.r, c.g, c.b, c.a };
        for (int i = 0; i < n; i++) {
            v[2 * i].position = po[i]; v[2 * i + 1].position = pi[i];
            v[2 * i].color = v[2 * i + 1].color = col; v[2 * i].tex_coord.x = v[2 * i].tex_coord.y = v[2 * i + 1].tex_coord.x = v[2 * i + 1].tex_coord.y = 0;
        }
        for (int i = 0; i < n; i++) {
            int a = 2 * i, b = 2 * i + 1, c2 = 2 * ((i + 1) % n), d = 2 * ((i + 1) % n) + 1;
            idx[ni++] = a; idx[ni++] = c2; idx[ni++] = b; idx[ni++] = b; idx[ni++] = c2; idx[ni++] = d;
        }
        SDL_RenderGeometry(R, NULL, v, 2 * n, idx, ni);
        return;
    }
#endif
    SDL_Rect e[4] = { { r.x, r.y, r.w, t }, { r.x, r.y + r.h - t, r.w, t }, { r.x, r.y, t, r.h }, { r.x + r.w - t, r.y, t, r.h } };
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a); SDL_RenderFillRects(R, e, 4);
}
static void grad(SDL_Rect r, UiCol a, UiCol b, int vert) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    SDL_Vertex v[4]; SDL_Color ca = { a.r, a.g, a.b, a.a }, cb = { b.r, b.g, b.b, b.a };
    float x0 = (float)r.x, y0 = (float)r.y, x1 = (float)(r.x + r.w), y1 = (float)(r.y + r.h);
    v[0].position.x = x0; v[0].position.y = y0; v[1].position.x = x1; v[1].position.y = y0;
    v[2].position.x = x1; v[2].position.y = y1; v[3].position.x = x0; v[3].position.y = y1;
    v[0].color = ca; v[1].color = vert ? ca : cb; v[2].color = cb; v[3].color = vert ? cb : ca;
    for (int i = 0; i < 4; i++) v[i].tex_coord.x = v[i].tex_coord.y = 0;
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(R, NULL, v, 4, idx, 6);
#else
    (void)vert; UiCol m = { (Uint8)((a.r + b.r) / 2), (Uint8)((a.g + b.g) / 2), (Uint8)((a.b + b.b) / 2), (Uint8)((a.a + b.a) / 2) }; ui_fill(r, m);
#endif
}
void ui_vgrad(SDL_Rect r, UiCol top, UiCol bottom) { grad(r, top, bottom, 1); }
void ui_hgrad(SDL_Rect r, UiCol left, UiCol right) { grad(r, left, right, 0); }
SDL_Surface* ui_image_surface(const void* data, int n) {
    int w, h, ch; unsigned char* px = stbi_load_from_memory(data, n, &w, &h, &ch, 4);
    if (!px) return NULL;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (s) for (int y = 0; y < h; y++) memcpy((char*)s->pixels + (size_t)y * s->pitch, px + (size_t)y * w * 4, (size_t)w * 4);
    stbi_image_free(px);
    return s;
}
SDL_Texture* ui_image(const void* data, int n, int* w, int* h) {
    SDL_Surface* s = ui_image_surface(data, n); if (!s) return NULL;
    SDL_Texture* t = SDL_CreateTextureFromSurface(R, s);
    if (w) *w = s->w; if (h) *h = s->h;
    SDL_FreeSurface(s);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
#endif
    return t;
}
void ui_image_cover(SDL_Texture* t, int iw, int ih, SDL_Rect d, float fy) {
    if (!t || iw <= 0 || ih <= 0 || d.w <= 0 || d.h <= 0) return;
    SDL_Rect s;
    if ((double)iw / ih > (double)d.w / d.h) { s.h = ih; s.w = (int)((double)ih * d.w / d.h); s.x = (iw - s.w) / 2; s.y = 0; }
    else { s.w = iw; s.h = (int)((double)iw * d.h / d.w); s.x = 0; s.y = (int)((ih - s.h) * fy); }
    SDL_RenderCopy(R, t, &s, &d);
}
static SDL_Rect g_clip[8]; static int g_nclip;
static SDL_Rect isect(SDL_Rect a, SDL_Rect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    SDL_Rect r = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 }; return r;
}
void ui_clip_push(SDL_Rect r) {
    if (g_nclip > 0) r = isect(r, g_clip[g_nclip - 1]);
    if (g_nclip < 8) g_clip[g_nclip++] = r;
    SDL_RenderSetClipRect(R, &r);
}
void ui_clip_pop(void) {
    if (g_nclip > 0) g_nclip--;
    if (g_nclip) SDL_RenderSetClipRect(R, &g_clip[g_nclip - 1]); else SDL_RenderSetClipRect(R, NULL);
}

/* ------------------------------------------------------------------------------------------- core --- */
static SDL_Cursor* g_cur[3]; static int g_curset = -1;
int ui_init(SDL_Renderer* r) {
    R = r;
    if (!F[0].ok) {
        font_init(&F[0], me1a_exo_reg); font_init(&F[1], me1a_exo_bold); font_init(&F[2], me1a_mono); font_init(&F[3], me1a_sym);
    }
    if (!F[0].ok) return 0;
    A = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, AW, AH);
    if (!A) { fprintf(stderr, "[ui] atlas: %s\n", SDL_GetError()); return 0; }
    SDL_SetTextureBlendMode(A, SDL_BLENDMODE_BLEND);
    atlas_reset();
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    if (ui.scale <= 0) ui.scale = 1;
    if (ui.ptscale <= 0) ui.ptscale = 1;
    return 1;
}
void ui_shutdown(void) {
    if (A) SDL_DestroyTexture(A);
    A = NULL; R = NULL;
    for (int i = 0; i < 3; i++) if (g_cur[i]) { SDL_FreeCursor(g_cur[i]); g_cur[i] = NULL; }
    g_curset = -1;
}
uint32_t ui_id(const char* s, int k) {
    uint32_t h = 2166136261u ^ (uint32_t)k * 16777619u;
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}
void ui_event(const SDL_Event* e) {
    switch (e->type) {
    case SDL_MOUSEMOTION: ui.mx = (int)(e->motion.x * ui.ptscale); ui.my = (int)(e->motion.y * ui.ptscale); break;
    case SDL_MOUSEBUTTONDOWN:
        ui.mx = (int)(e->button.x * ui.ptscale); ui.my = (int)(e->button.y * ui.ptscale);
        if (e->button.button == SDL_BUTTON_LEFT) { ui.down = 1; ui.pressed = 1; } else if (e->button.button == SDL_BUTTON_RIGHT) ui.rpressed = 1;
        break;
    case SDL_MOUSEBUTTONUP:
        ui.mx = (int)(e->button.x * ui.ptscale); ui.my = (int)(e->button.y * ui.ptscale);
        if (e->button.button == SDL_BUTTON_LEFT) { ui.down = 0; ui.released = 1; }
        break;
    case SDL_MOUSEWHEEL: ui.wheel += e->wheel.y * (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1); break;
    case SDL_KEYDOWN:
        if (ui.nkeys < 32) ui.keys[ui.nkeys++] = (int)e->key.keysym.sym | ((e->key.keysym.mod & KMOD_CTRL) ? UI_CTRL : 0);
        break;
    case SDL_TEXTINPUT: { size_t l = strlen(ui.text); snprintf(ui.text + l, sizeof ui.text - l, "%s", e->text.text); break; }
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_LEAVE) { ui.mx = ui.my = -10000; }
        break;
    }
}
void ui_begin(SDL_Window* w) {
    int ow = 0, oh = 0, ww = 0, wh = 0;
    SDL_GetRendererOutputSize(R, &ow, &oh); SDL_GetWindowSize(w, &ww, &wh);
    ui.w = ow; ui.h = oh; ui.ptscale = ww > 0 ? (float)ow / (float)ww : 1.0f;
    ui.now = SDL_GetTicks(); ui.cursor = 0; ui.hot = 0;
    ui.layer = UI_LAYER_MAIN;
}
void ui_end(void) {
    if (ui.released) ui.active = 0;
    ui.pressed = ui.released = ui.rpressed = 0; ui.wheel = 0; ui.nkeys = 0; ui.text[0] = 0;
    if (ui.cursor != g_curset) {
        static const SDL_SystemCursor sc[3] = { SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_HAND, SDL_SYSTEM_CURSOR_IBEAM };
        if (!g_cur[ui.cursor]) g_cur[ui.cursor] = SDL_CreateSystemCursor(sc[ui.cursor]);
        if (g_cur[ui.cursor]) SDL_SetCursor(g_cur[ui.cursor]);
        g_curset = ui.cursor;
    }
}
int ui_key(int key) {
    for (int i = 0; i < ui.nkeys; i++) if (ui.keys[i] == key) { memmove(&ui.keys[i], &ui.keys[i + 1], sizeof(int) * (size_t)(ui.nkeys - i - 1)); ui.nkeys--; return 1; }
    return 0;
}
int ui_hover(SDL_Rect r) {
    if (ui.layer != ui.top) return 0;
    if (g_nclip) { SDL_Rect c = g_clip[g_nclip - 1]; if (ui.mx < c.x || ui.my < c.y || ui.mx >= c.x + c.w || ui.my >= c.y + c.h) return 0; }
    return ui.mx >= r.x && ui.my >= r.y && ui.mx < r.x + r.w && ui.my < r.y + r.h;
}
/* press + release on the same widget */
static int clicked(uint32_t id, SDL_Rect r, int* hov) {
    *hov = ui_hover(r);
    if (*hov) { ui.hot = id; ui.cursor = 1; if (ui.pressed) ui.active = id; }
    if (ui.active == id && ui.released) { ui.active = 0; if (*hov) { ui.redraw = 1; return 1; } }
    return 0;
}

/* ---------------------------------------------------------------------------------------- widgets --- */
int ui_button(uint32_t id, SDL_Rect r, const char* label, int style) {
    int dis = style & UB_DISABLED, hov = 0, c = 0; style &= 0xFF;
    if (!dis) c = clicked(id, r, &hov);
    int held = ui.active == id && ui.down && hov, rad = D(6);
    int fpx = D(style == UB_PRIMARY ? 17 : 14), face = style == UB_PRIMARY ? UF_BOLD : UF_REG;
    UiCol tc = UIC(C_TEXT);
    if (dis && style == UB_PRIMARY) {   /* unavailable main action: muted, readable */
        ui_rrect(r, rad, UIC(0x16243a)); ui_rframe(r, rad, 1, UIC(0x2a4060));
        ui_text_c(face, fpx, r, UIC(0x6f86a3), label);
        return 0;
    }
    switch (style) {
    case UB_PRIMARY: {
        SDL_Rect g = r;
        ui_rrect(g, rad, UIC(held ? 0x1a5fb8 : hov ? 0x2f86f0 : 0x2373dc));
        SDL_Rect hl = { r.x + rad, r.y + 1, r.w - 2 * rad, r.h / 2 };
        ui_vgrad(hl, UICA(0xffffff, hov ? 46 : 30), UICA(0xffffff, 0));
        ui_rframe(r, rad, D(1) > 0 ? D(1) : 1, UIC(hov ? C_ACC2 : 0x5fb4ff));
        tc = UIC(0xffffff);
        break; }
    case UB_FLAT:
        if (hov) ui_rrect(r, rad, UICA(C_ACC, 30));
        tc = UIC(hov ? C_ACC2 : C_ACC);
        break;
    case UB_DANGER:
        ui_rrect(r, rad, UIC(held ? 0x5a1c22 : hov ? 0x7a2630 : 0x4a1a20)); ui_rframe(r, rad, 1, UIC(0xa0404a));
        break;
    case UB_TAB: case UB_TABSEL: {
        int sel = style == UB_TABSEL;
        if (sel) { ui_hgrad(r, UICA(C_ACC, 70), UICA(C_ACC, 8)); SDL_Rect bar = { r.x, r.y, D(3), r.h }; ui_fill(bar, UIC(C_ACC)); }
        else if (hov) ui_fill(r, UICA(C_ACC, 22));
        tc = UIC(sel ? 0xffffff : hov ? C_TEXT : 0xb4c3d4);
        int lh = ui_lineh(UF_REG, D(15));
        ui_text_fit(sel ? UF_BOLD : UF_REG, D(15), r.x + D(18), r.y + (r.h - lh) / 2, r.w - D(24), tc, label);
        return c; }
    default:
        ui_rrect(r, rad, UIC(held ? 0x14243a : hov ? 0x1d3350 : C_PANEL2));
        ui_rframe(r, rad, 1, UIC(hov ? C_ACC : C_LINE));
        break;
    }
    if (dis) tc = UIC(0x5a6a7e);
    SDL_Rect tr = r; if (held) tr.y += 1;
    ui_text_c(face, fpx, tr, tc, label);
    return c;
}
static void draw_switch(SDL_Rect s, int on, int hov, int dis) {
    ui_rrect(s, s.h / 2, UIC(dis ? 0x1a2230 : on ? (hov ? 0x3aa6ff : 0x2a8ae6) : (hov ? 0x2b3a4f : 0x223042)));
    int k = s.h - D(6); SDL_Rect kn = { on ? s.x + s.w - k - D(3) : s.x + D(3), s.y + D(3), k, k };
    ui_rrect(kn, k / 2, UIC(dis ? 0x45505e : on ? 0xffffff : 0x9fb0c4));
}
int ui_switch(uint32_t id, SDL_Rect row, const char* label, int on, int disabled) {
    int hov = 0, c = disabled ? 0 : clicked(id, row, &hov);
    SDL_Rect s = { row.x + row.w - D(46), row.y + (row.h - D(24)) / 2, D(46), D(24) };
    if (hov) ui_rrect(row, D(6), UICA(C_ACC, 14));
    int lh = ui_lineh(UF_REG, D(15));
    ui_text_fit(UF_REG, D(15), row.x + D(4), row.y + (row.h - lh) / 2, row.w - D(60), UIC(disabled ? 0x6a7a8e : C_TEXT), label);
    draw_switch(s, on, hov, disabled);
    return c;
}
int ui_check(uint32_t id, SDL_Rect row, const char* label, int on) {
    int hov = 0, c = clicked(id, row, &hov), b = D(20);
    SDL_Rect box = { row.x + D(4), row.y + (row.h - b) / 2, b, b };
    if (hov) ui_rrect(row, D(6), UICA(C_ACC, 14));
    ui_rrect(box, D(4), UIC(on ? 0x2a8ae6 : 0x1a2636)); ui_rframe(box, D(4), 1, UIC(on ? C_ACC2 : hov ? C_ACC : C_LINE));
    if (on) ui_text_c(UF_BOLD, D(15), box, UIC(0xffffff), "✓");
    int lh = ui_lineh(UF_REG, D(15));
    ui_text_fit(UF_REG, D(15), box.x + b + D(10), row.y + (row.h - lh) / 2, row.w - b - D(20), UIC(C_TEXT), label);
    return c;
}

/* combo: the list is a popup drawn by ui_overlay(); the choice comes back on the next frame */
static struct { uint32_t id, rid; int res, n, sel, scroll, layer, fresh; SDL_Rect anchor; char* items[128]; } P;
int ui_popup_open(void) { return P.id != 0; }
void ui_popup_close(void) { for (int i = 0; i < P.n; i++) free(P.items[i]); P.n = 0; P.id = 0; ui.redraw = 1; }
int ui_combo(uint32_t id, SDL_Rect r, const char* const* labels, int n, int* sel) {
    int changed = 0;
    if (P.rid == id) { if (P.res >= 0 && P.res < n && P.res != *sel) { *sel = P.res; changed = 1; } P.rid = 0; }
    int hov = 0, c = clicked(id, r, &hov), open = P.id == id;
    ui_rrect(r, D(6), UIC(open ? 0x14243a : hov ? 0x1a2c44 : C_PANEL2));
    ui_rframe(r, D(6), 1, UIC(open || hov ? C_ACC : C_LINE));
    int lh = ui_lineh(UF_REG, D(14));
    const char* cur = *sel >= 0 && *sel < n ? labels[*sel] : "—";
    ui_text_fit(UF_REG, D(14), r.x + D(12), r.y + (r.h - lh) / 2, r.w - D(40), UIC(C_TEXT), cur);
    SDL_Rect ar = { r.x + r.w - D(30), r.y, D(26), r.h }; ui_text_c(UF_REG, D(14), ar, UIC(C_ACC), open ? "▴" : "▾");
    if (c && !open) {
        ui_popup_close();
        P.id = id; P.anchor = r; P.sel = *sel; P.n = n > 128 ? 128 : n; P.scroll = -1; P.layer = ui.layer; P.fresh = 1;
        for (int i = 0; i < P.n; i++) P.items[i] = strdup(labels[i]);
    } else if (c && open) ui_popup_close();
    return changed;
}
static void popup_draw(void) {
    if (!P.id) return;
    int ih = D(30), vis = P.n < 14 ? P.n : 14, w = P.anchor.w, maxw = 0;
    for (int i = 0; i < P.n; i++) { int tw = ui_textw(UF_REG, D(14), P.items[i], -1) + D(36); if (tw > maxw) maxw = tw; }
    if (maxw > w) w = maxw; if (w > ui.w - D(16)) w = ui.w - D(16);
    SDL_Rect box = { P.anchor.x, P.anchor.y + P.anchor.h + D(2), w, vis * ih + D(8) };
    if (box.y + box.h > ui.h - D(8)) { int up = P.anchor.y - box.h - D(2); if (up >= D(8)) box.y = up; else box.y = ui.h - D(8) - box.h; }
    if (box.x + box.w > ui.w - D(8)) box.x = ui.w - D(8) - box.w;
    if (P.scroll < 0) { P.scroll = P.sel - vis / 2; }
    if (P.scroll > P.n - vis) P.scroll = P.n - vis; if (P.scroll < 0) P.scroll = 0;
    ui.layer = UI_LAYER_POPUP;
    SDL_Rect sh = { box.x + D(3), box.y + D(5), box.w, box.h }; ui_rrect(sh, D(8), UICA(0, 120));
    ui_rrect(box, D(8), UIC(0x0f1824)); ui_rframe(box, D(8), 1, UIC(C_ACC));
    if (ui_hover(box) && ui.wheel) { P.scroll -= ui.wheel; ui.wheel = 0; if (P.scroll > P.n - vis) P.scroll = P.n - vis; if (P.scroll < 0) P.scroll = 0; ui.redraw = 1; }
    for (int k = 0; k < vis; k++) {
        int i = P.scroll + k; SDL_Rect it = { box.x + D(4), box.y + D(4) + k * ih, box.w - D(8), ih };
        int hov = ui_hover(it);
        if (hov) { ui_rrect(it, D(5), UICA(C_ACC, 60)); ui.cursor = 1; }
        if (i == P.sel) { SDL_Rect bar = { it.x, it.y + D(6), D(3), ih - D(12) }; ui_fill(bar, UIC(C_ACC)); }
        int lh = ui_lineh(UF_REG, D(14));
        ui_text_fit(i == P.sel ? UF_BOLD : UF_REG, D(14), it.x + D(12), it.y + (ih - lh) / 2, it.w - D(18), UIC(hov || i == P.sel ? 0xffffff : C_TEXT), P.items[i]);
        if (hov && ui.released && !P.fresh && ui.active != 0x7fffffff) { P.rid = P.id; P.res = i; ui_popup_close(); ui.released = 0; ui.active = 0; return; }
    }
    if (P.n > vis) {   /* scroll hint */
        SDL_Rect tr = { box.x + box.w - D(5), box.y + D(4), D(3), box.h - D(8) }; ui_fill(tr, UICA(C_LINE, 160));
        SDL_Rect th = { tr.x, tr.y + tr.h * P.scroll / P.n, D(3), tr.h * vis / P.n }; ui_fill(th, UIC(C_ACC));
    }
    if (ui.pressed && !ui_hover(box) && !P.fresh) { ui_popup_close(); ui.pressed = 0; ui.active = 0x7fffffff; return; }   /* click outside: close, eat it */
    if (ui_key(SDLK_ESCAPE)) { ui_popup_close(); return; }
    P.fresh = 0;
}

int ui_slider(uint32_t id, SDL_Rect r, double* v, double min, double max, double step) {
    int hov = ui_hover(r), changed = 0;
    if (hov) { ui.hot = id; ui.cursor = 1; if (ui.pressed) ui.active = id; }
    SDL_Rect tr = { r.x + D(8), r.y + r.h / 2 - D(2), r.w - D(16), D(4) };
    if (ui.active == id && (ui.down || ui.released) && max > min) {
        double t = (double)(ui.mx - tr.x) / (tr.w > 0 ? tr.w : 1); if (t < 0) t = 0; if (t > 1) t = 1;
        double nv = min + t * (max - min); if (step > 0) nv = min + floor((nv - min) / step + 0.5) * step;
        if (nv > max) nv = max; if (nv < min) nv = min;
        if (fabs(nv - *v) > 1e-9) { *v = nv; changed = 1; ui.redraw = 1; }
    }
    if (hov && ui.wheel && max > min) {
        double nv = *v + ui.wheel * (step > 0 ? step : (max - min) / 20); ui.wheel = 0;
        if (nv > max) nv = max; if (nv < min) nv = min; if (nv != *v) { *v = nv; changed = 1; }
    }
    double t = max > min ? (*v - min) / (max - min) : 0; if (t < 0) t = 0; if (t > 1) t = 1;
    ui_rrect(tr, D(2), UIC(0x223042));
    SDL_Rect fl = { tr.x, tr.y, (int)(tr.w * t), tr.h }; ui_rrect(fl, D(2), UIC(C_ACC));
    int k = D(16); SDL_Rect kn = { tr.x + (int)(tr.w * t) - k / 2, r.y + r.h / 2 - k / 2, k, k };
    ui_rrect(kn, k / 2, UIC(ui.active == id || hov ? 0xffffff : 0xcfe6ff));
    return changed;
}

/* single-line text field */
static int g_caret = -1; static uint32_t g_caret_id;
static int u8prev(const char* s, int i) { if (i <= 0) return 0; i--; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--; return i; }
static int u8fwd(const char* s, int i) { if (!s[i]) return i; i++; while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) i++; return i; }
int ui_input(uint32_t id, SDL_Rect r, char* buf, int cap, const char* ph) {
    int hov = ui_hover(r), res = 0, len = (int)strlen(buf), px = D(14);
    if (hov) ui.cursor = 2;
    if (hov && ui.pressed) { if (ui.focus != id) { ui.focus = id; SDL_StartTextInput(); } g_caret_id = id; g_caret = -2; }
    else if (ui.pressed && ui.focus == id && ui.layer == ui.top) { ui.focus = 0; SDL_StopTextInput(); }
    int foc = ui.focus == id;
    if (foc && g_caret_id != id) { g_caret_id = id; g_caret = len; }
    if (foc) {
        if (g_caret < 0 || g_caret > len) {
            if (g_caret == -2) {   /* click: caret at the mouse */
                int x0 = r.x + D(10), best = len, i = 0;
                int tw = ui_textw(UF_REG, px, buf, len), off = tw > r.w - D(20) ? tw - (r.w - D(20)) : 0;
                for (;;) { int w = ui_textw(UF_REG, px, buf, i); if (x0 + w - off >= ui.mx) { best = i; break; } if (!buf[i]) break; i = u8fwd(buf, i); }
                g_caret = best;
            } else g_caret = len;
        }
        if (ui.text[0]) {
            int tl = (int)strlen(ui.text);
            if (len + tl < cap) { memmove(buf + g_caret + tl, buf + g_caret, (size_t)(len - g_caret + 1)); memcpy(buf + g_caret, ui.text, (size_t)tl); g_caret += tl; len += tl; res = 1; }
            ui.text[0] = 0;
        }
        for (int k = 0; k < ui.nkeys; k++) {
            int key = ui.keys[k], used = 1;
            if (key == SDLK_BACKSPACE && g_caret > 0) { int p = u8prev(buf, g_caret); memmove(buf + p, buf + g_caret, (size_t)(len - g_caret + 1)); len -= g_caret - p; g_caret = p; res = 1; }
            else if (key == SDLK_DELETE && g_caret < len) { int n = u8fwd(buf, g_caret); memmove(buf + g_caret, buf + n, (size_t)(len - n + 1)); len -= n - g_caret; res = 1; }
            else if (key == SDLK_LEFT) g_caret = u8prev(buf, g_caret);
            else if (key == SDLK_RIGHT) g_caret = u8fwd(buf, g_caret);
            else if (key == SDLK_HOME) g_caret = 0;
            else if (key == SDLK_END) g_caret = len;
            else if (key == (SDLK_v | UI_CTRL)) {
                char* cb = SDL_GetClipboardText();
                if (cb) { for (char* q = cb; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
                    int tl = (int)strlen(cb); if (len + tl < cap) { memmove(buf + g_caret + tl, buf + g_caret, (size_t)(len - g_caret + 1)); memcpy(buf + g_caret, cb, (size_t)tl); g_caret += tl; len += tl; res = 1; }
                    SDL_free(cb); }
            }
            else if (key == (SDLK_c | UI_CTRL)) SDL_SetClipboardText(buf);
            else if (key == (SDLK_a | UI_CTRL)) { buf[0] = 0; len = 0; g_caret = 0; res = 1; }   /* Ctrl+A: clear (no selection support) */
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) { res = 2; }
            else if (key == SDLK_ESCAPE) { ui.focus = 0; SDL_StopTextInput(); }
            else used = 0;
            if (used) { memmove(&ui.keys[k], &ui.keys[k + 1], sizeof(int) * (size_t)(ui.nkeys - k - 1)); ui.nkeys--; k--; ui.redraw = 1; }
        }
    }
    ui_rrect(r, D(6), UIC(0x0d1520)); ui_rframe(r, D(6), 1, UIC(foc ? C_ACC : hov ? 0x37506f : C_LINE));
    int lh = ui_lineh(UF_REG, px), ty = r.y + (r.h - lh) / 2, x0 = r.x + D(10);
    SDL_Rect in = { r.x + D(6), r.y + 2, r.w - D(12), r.h - 4 };
    ui_clip_push(in);
    if (!buf[0] && !foc && ph) ui_text(UF_REG, px, x0, ty, UIC(0x5d6e83), ph, -1);
    int cw = foc ? ui_textw(UF_REG, px, buf, g_caret) : 0, off = 0, avail = r.w - D(20);
    if (foc && cw > avail) off = cw - avail;
    else if (!foc) { int tw = ui_textw(UF_REG, px, buf, -1); if (tw > avail) off = 0; }
    ui_text(UF_REG, px, x0 - off, ty, UIC(C_TEXT), buf, -1);
    if (foc) { SDL_Rect c = { x0 - off + cw, ty + D(2), D(2) > 0 ? D(2) : 1, lh - D(4) }; ui_fill(c, UIC(C_ACC2)); }
    ui_clip_pop();
    return res;
}

/* vertical scroll area: wheel over the view, scrollbar drag; content height from the previous frame */
void ui_scroll_begin(UiScroll* s, SDL_Rect v) {
    s->view = v.h; s->vr = v;
    if (ui_hover(v) && ui.wheel) { s->pos -= ui.wheel * D(60); ui.wheel = 0; ui.redraw = 1; }
    int maxp = s->content - v.h; if (maxp < 0) maxp = 0;
    if (s->pos > maxp) s->pos = maxp; if (s->pos < 0) s->pos = 0;
    ui_clip_push(v);
}
void ui_scroll_to_end(UiScroll* s) { s->pos = 1 << 28; }
void ui_scroll_end(UiScroll* s, int content_h) {
    ui_clip_pop();
    if (s->content != content_h) ui.redraw = 1;
    s->content = content_h;
    SDL_Rect v = s->vr;
    int maxp = s->content - v.h; if (maxp <= 0) return;
    if (s->pos > maxp) s->pos = maxp;
    uint32_t id = s->id ? s->id : (uint32_t)(uintptr_t)s;
    SDL_Rect tr = { v.x + v.w - D(8), v.y + D(4), D(5), v.h - D(8) };
    int th = (int)((long long)tr.h * v.h / s->content); if (th < D(30)) th = D(30);
    int ty = tr.y + (int)((long long)(tr.h - th) * s->pos / maxp);
    SDL_Rect thumb = { tr.x - D(3), ty, tr.w + D(6), th };
    int hov = ui_hover(thumb);
    if (hov) ui.cursor = 1;
    if (hov && ui.pressed) { ui.active = id; s->grab = ui.my - ty; }
    if (ui.active == id && ui.down) {
        int ny = ui.my - s->grab - tr.y; s->pos = (int)((long long)ny * maxp / (tr.h - th > 0 ? tr.h - th : 1));
        if (s->pos < 0) s->pos = 0; if (s->pos > maxp) s->pos = maxp; ui.redraw = 1;
    }
    ui_rrect(tr, D(2), UICA(C_LINE, 120));
    SDL_Rect tv = { tr.x, ty, tr.w, th }; ui_rrect(tv, D(2), UIC(ui.active == id || hov ? C_ACC2 : C_ACC));
}

static char g_toast[512]; static uint32_t g_toast_t;
void ui_toast(const char* msg) { snprintf(g_toast, sizeof g_toast, "%s", msg); g_toast_t = SDL_GetTicks(); ui.redraw = 1; }
void ui_overlay(void) {
    popup_draw();
    ui.layer = UI_LAYER_POPUP;
    if (g_toast[0]) {
        uint32_t age = SDL_GetTicks() - g_toast_t;
        if (age > 4500) g_toast[0] = 0;
        else {
            int px = D(14), mw = ui.w * 2 / 3, th = ui_textbox_h(UF_REG, px, mw, g_toast);
            static UiLine L[32]; int n = ui_wrap(UF_REG, px, g_toast, mw, L, 32), w = 0;
            for (int i = 0; i < n; i++) { int lw = ui_textw(UF_REG, px, L[i].s, L[i].n); if (lw > w) w = lw; }
            SDL_Rect b = { (ui.w - w) / 2 - D(18), ui.h - th - D(40), w + D(36), th + D(20) };
            int a = age > 4000 ? (int)(255 * (4500 - age) / 500) : 255;
            ui_rrect(b, D(8), UICA(0x0c1622, a * 240 / 255)); ui_rframe(b, D(8), 1, UICA(C_ACC, a));
            ui_textbox(UF_REG, px, b.x + D(18), b.y + D(10), mw, UICA(C_TEXT, a), g_toast);
            ui.redraw = 1;
        }
    }
}
