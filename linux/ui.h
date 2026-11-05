/* linux/ui.h - a small immediate-mode UI on SDL_Renderer (v1.1): text (stb_truetype, embedded Exo 2 + DejaVu),
 * images (stb_image), widgets for me1-launcher and the loading screen of me1-linux. All coordinates are pixels of the
 * renderer output; ui.scale converts design units (dp) to pixels: D(x). */
#pragma once
#include <SDL.h>
#include <stdint.h>

typedef struct { Uint8 r, g, b, a; } UiCol;
#define UIC(h) ((UiCol){ (Uint8)((h) >> 16 & 255), (Uint8)((h) >> 8 & 255), (Uint8)((h) & 255), 255 })
#define UICA(h, al) ((UiCol){ (Uint8)((h) >> 16 & 255), (Uint8)((h) >> 8 & 255), (Uint8)((h) & 255), (Uint8)(al) })
/* theme (Mass Effect: dark navy, holo blue) */
#define C_BG     0x080c12
#define C_SIDE   0x0b1119
#define C_PANEL  0x111a26
#define C_PANEL2 0x172334
#define C_LINE   0x253852
#define C_TEXT   0xdfe8f2
#define C_DIM    0x8d9fb5
#define C_ACC    0x3aa6ff
#define C_ACC2   0x8fd6ff
#define C_WARN   0xf0b040
#define C_OK     0x8bd36b
#define C_ERR    0xff6b6b

enum { UF_REG, UF_BOLD, UF_MONO, UF_N };
enum { UB_NORMAL, UB_PRIMARY, UB_FLAT, UB_DANGER, UB_TAB, UB_TABSEL, UB_DISABLED = 0x100 };
enum { UI_LAYER_MAIN, UI_LAYER_MODAL, UI_LAYER_POPUP };

typedef struct {
    int mx, my, down, pressed, released, wheel, rpressed;
    int keys[32], nkeys; char text[128];   /* this frame: key presses (SDL_Keycode | mod << 24 not used), SDL_TEXTINPUT */
    uint32_t hot, active, focus;
    float scale;              /* pixels per dp */
    float ptscale;            /* output pixels per window point (HiDPI) */
    int w, h;                 /* output size */
    int redraw;               /* another frame wanted (state changed while drawing) */
    int layer, top;           /* layer being drawn / layer that gets the mouse */
    uint32_t now;
    int cursor;               /* wanted system cursor this frame */
} Ui;
extern Ui ui;
#define D(x) ((int)((x) * ui.scale + 0.5f))
#define UI_CTRL 0x10000000   /* ui.keys[]: Ctrl held */

int  ui_init(SDL_Renderer* r);        /* fonts, glyph atlas; 0 on failure */
void ui_shutdown(void);               /* frees textures (before SDL_DestroyRenderer) */
void ui_event(const SDL_Event* e);    /* feed every SDL event */
void ui_begin(SDL_Window* w);         /* start a frame: output size, cursor */
void ui_end(void);                    /* end of the frame: reset one-frame input, cursor shape */
uint32_t ui_id(const char* s, int k);

/* text: px = font size in pixels; y = top of the line */
int  ui_lineh(int face, int px);
int  ui_textw(int face, int px, const char* s, int n);
int  ui_text(int face, int px, int x, int y, UiCol c, const char* s, int n);   /* -> end x */
int  ui_text_fit(int face, int px, int x, int y, int maxw, UiCol c, const char* s);   /* ellipsis */
typedef struct { const char* s; int n; } UiLine;
int  ui_wrap(int face, int px, const char* s, int maxw, UiLine* out, int max);
int  ui_textbox(int face, int px, int x, int y, int maxw, UiCol c, const char* s);   /* wrapped; -> height */
int  ui_textbox_h(int face, int px, int maxw, const char* s);
int  ui_text_c(int face, int px, SDL_Rect r, UiCol c, const char* s);   /* centred in r (one line, fitted) */

/* drawing */
void ui_fill(SDL_Rect r, UiCol c);
void ui_rrect(SDL_Rect r, int rad, UiCol c);
void ui_rframe(SDL_Rect r, int rad, int t, UiCol c);
void ui_vgrad(SDL_Rect r, UiCol top, UiCol bottom);
void ui_hgrad(SDL_Rect r, UiCol left, UiCol right);
SDL_Texture* ui_image(const void* data, int n, int* w, int* h);
SDL_Surface* ui_image_surface(const void* data, int n);
void ui_image_cover(SDL_Texture* t, int iw, int ih, SDL_Rect dst, float fy);   /* centre-crop to dst; fy: vertical focus 0..1 */
void ui_clip_push(SDL_Rect r);
void ui_clip_pop(void);

/* widgets: return 1 when used */
int  ui_hover(SDL_Rect r);
int  ui_button(uint32_t id, SDL_Rect r, const char* label, int style);
int  ui_switch(uint32_t id, SDL_Rect row, const char* label, int on, int disabled);   /* label left, switch right */
int  ui_check(uint32_t id, SDL_Rect row, const char* label, int on);
int  ui_combo(uint32_t id, SDL_Rect r, const char* const* labels, int n, int* sel);
int  ui_slider(uint32_t id, SDL_Rect r, double* v, double min, double max, double step);
int  ui_input(uint32_t id, SDL_Rect r, char* buf, int cap, const char* placeholder);   /* 1 changed, 2 Enter */
typedef struct { int pos, content, view; uint32_t id; int grab; SDL_Rect vr; } UiScroll;
void ui_scroll_begin(UiScroll* s, SDL_Rect view);
void ui_scroll_end(UiScroll* s, int content_h);   /* also draws the scrollbar */
void ui_scroll_to_end(UiScroll* s);
int  ui_key(int key);                 /* pressed this frame (consumes) */
void ui_toast(const char* msg);
void ui_overlay(void);                /* popups (combo lists) and the toast: call last */
int  ui_popup_open(void);
void ui_popup_close(void);
