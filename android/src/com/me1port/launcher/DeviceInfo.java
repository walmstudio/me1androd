package com.me1port.launcher;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.InputStreamReader;

/** Device facts for the automatic graphics preset (v0.9.7): GPU (GL_RENDERER of a 1x1 offscreen GLES context, cached
 * per system build), SoC (ro.soc.model / ro.board.platform / Build.HARDWARE), RAM, CPU cores and top frequency, screen. */
final class DeviceInfo {
    String gpu = "", glVersion = "", soc = "", socModel = "", platform = "";
    int ramMb, cores, maxMhz, screenW, screenH;

    static DeviceInfo get(Activity a, SharedPreferences prefs) {
        DeviceInfo d = new DeviceInfo();
        String key = "gl_" + Build.FINGERPRINT.hashCode(), cached = prefs.getString(key, null);
        if (cached != null && cached.indexOf('\t') >= 0) { d.gpu = cached.substring(0, cached.indexOf('\t')); d.glVersion = cached.substring(cached.indexOf('\t') + 1); }
        else {
            String[] g = queryGl();
            if (g != null) { d.gpu = g[0]; d.glVersion = g[1]; prefs.edit().putString(key, d.gpu + "\t" + d.glVersion).apply(); }
        }
        d.socModel = prop("ro.soc.model"); d.platform = prop("ro.board.platform");
        d.soc = !d.socModel.isEmpty() ? d.socModel : !d.platform.isEmpty() ? d.platform : Build.HARDWARE;
        try {
            ActivityManager am = (ActivityManager) a.getSystemService(Context.ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo(); am.getMemoryInfo(mi);
            d.ramMb = (int) (mi.totalMem >> 20);
        } catch (Exception ignored) { }
        d.cores = Runtime.getRuntime().availableProcessors();
        for (int i = 0; i < 16; i++) {
            File f = new File("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq");
            if (!f.exists()) continue;
            try (BufferedReader r = new BufferedReader(new FileReader(f))) { d.maxMhz = Math.max(d.maxMhz, Integer.parseInt(r.readLine().trim()) / 1000); }
            catch (Exception ignored) { }
        }
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        a.getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        d.screenW = Math.max(dm.widthPixels, dm.heightPixels); d.screenH = Math.min(dm.widthPixels, dm.heightPixels);
        return d;
    }
    /** "tier\nreport" from the native classifier (apply: also set the values in the loaded config) */
    String classify(boolean apply) { return L.tx(Native.autoPreset(gpu, soc, ramMb, cores, maxMhz, screenW, screenH, apply ? 1 : 0)); }

    private static String prop(String name) {
        try {
            Process p = Runtime.getRuntime().exec(new String[] { "getprop", name });
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) { String l = r.readLine(); return l == null ? "" : l.trim(); }
        } catch (Exception e) { return ""; }
    }
    /** GL_RENDERER + GL_VERSION of an offscreen ES 3 (else ES 2) context; null if EGL fails */
    private static String[] queryGl() {
        EGLDisplay dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (dpy == EGL14.EGL_NO_DISPLAY) return null;
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(dpy, ver, 0, ver, 1)) return null;
        EGLContext ctx = EGL14.EGL_NO_CONTEXT; EGLSurface surf = EGL14.EGL_NO_SURFACE;
        try {
            for (int es : new int[] { 3, 2 }) {
                int[] attr = { EGL14.EGL_RENDERABLE_TYPE, es == 3 ? 0x40 /* EGL_OPENGL_ES3_BIT_KHR */ : EGL14.EGL_OPENGL_ES2_BIT,
                        EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_NONE };
                EGLConfig[] cfg = new EGLConfig[1]; int[] n = new int[1];
                if (!EGL14.eglChooseConfig(dpy, attr, 0, cfg, 0, 1, n, 0) || n[0] < 1) continue;
                ctx = EGL14.eglCreateContext(dpy, cfg[0], EGL14.EGL_NO_CONTEXT, new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, es, EGL14.EGL_NONE }, 0);
                if (ctx == null || ctx == EGL14.EGL_NO_CONTEXT) { ctx = EGL14.EGL_NO_CONTEXT; continue; }
                surf = EGL14.eglCreatePbufferSurface(dpy, cfg[0], new int[] { EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE }, 0);
                if (surf == null || surf == EGL14.EGL_NO_SURFACE || !EGL14.eglMakeCurrent(dpy, surf, surf, ctx)) {
                    if (surf != null && surf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, surf);
                    EGL14.eglDestroyContext(dpy, ctx); ctx = EGL14.EGL_NO_CONTEXT; surf = EGL14.EGL_NO_SURFACE; continue;
                }
                String r = GLES20.glGetString(GLES20.GL_RENDERER), v = GLES20.glGetString(GLES20.GL_VERSION);
                return new String[] { r == null ? "" : r, v == null ? "" : v };
            }
            return null;
        } catch (Throwable t) { return null; }
        finally {
            EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (surf != null && surf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, surf);
            if (ctx != null && ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx);
        }
    }
}
