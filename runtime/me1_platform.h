/* v1.1: desktop window systems (Linux port). The front end (linux/me1_linux.c, SDL2) creates the window and hands its
 * native handles to the renderers; the runtime itself does not link against X11, Wayland or SDL.
 *   me1_platform_set(kind, display, visual) - before the D3D9 device is created: the GLES backend opens its EGL display on
 *       this platform (EGL_PLATFORM_X11_KHR / EGL_PLATFORM_WAYLAND_KHR), the Vulkan renderer enables VK_KHR_xlib_surface /
 *       VK_KHR_wayland_surface. Without it (tests, servers) both stay surfaceless/headless as before.
 *   me1gl_set_window(Me1Window*) - the window (NULL: gone). The front end keeps the structure alive and updates w/h and
 *       bumps gen when the window is resized; the presenters pick that up (Wayland: wl_egl_window_resize; Vulkan: new
 *       swap chain).
 * On Android the window is an ANativeWindow* and none of this is used. */
#ifndef ME1_PLATFORM_H
#define ME1_PLATFORM_H
enum { ME1_WS_NONE = 0, ME1_WS_X11 = 1, ME1_WS_WAYLAND = 2 };
typedef struct Me1Window {
    int kind;                /* ME1_WS_X11 / ME1_WS_WAYLAND */
    void* display;           /* X11: Display*, Wayland: struct wl_display* */
    unsigned long xwindow;   /* X11: Window */
    void* surface;           /* Wayland: struct wl_surface* */
    volatile int w, h;       /* drawable size in pixels */
    volatile int gen;        /* +1 on every resize */
} Me1Window;
void me1_platform_set(int kind, void* display, unsigned long x11_visual);
int  me1_platform_kind(void);
#endif
