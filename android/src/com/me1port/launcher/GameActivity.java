package com.me1port.launcher;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.graphics.Color;
import android.graphics.BitmapFactory;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.Locale;

/** The game itself (process ":game"): libme1game.so runs MassEffect.exe on its own threads, the GLES backend presents
 * frames into this SurfaceView. Touch = mouse (one finger: left button, second finger: right button), Back = Escape,
 * a hardware keyboard works as the PC keyboard; ControlsOverlay adds on-screen controls (stick, keys, look area). The process ends when the game exits.
 * v0.9.6: a start-up screen (until the first frame: elapsed time, D3D shaders created, HLSL compiles / cache hits,
 * GL programs) and afterwards a small "compiling shaders" badge while the game compiles or links shaders.
 * v0.9.7: loading picture behind the start-up screen; gamepads (Pad): physical controllers and the on-screen Xbox pad
 * feed the game's XInput pad 0 (profile "xinput", needs a controller mod) or emulate keyboard + mouse (profile "kbm");
 * rumble (XInputSetState) is played on the controller's vibrator or on the phone. */
public class GameActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("me1game"); }
    static native int nativeStart(String[] args, String[] env, String log);
    static native void nativeSurface(Surface s);
    static native void nativeTouch(int action, float x, float y);
    static native void nativeKey(int code, int down, int unicode);
    static native void nativeMouseRel(float dx, float dy);
    static native void nativeMouseButton(int button, int down);
    /** XInput pad 0: XINPUT_GAMEPAD buttons, triggers 0..255, sticks -32768..32767 (Y up = positive) */
    static native void nativePad(int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    static native void nativePadConnect(int on);
    /** (left motor << 16) | right motor, 0..65535 each (last XInputSetState) */
    static native int nativePadVibration();
    static final Pad pad = new Pad();
    /** see me1_status() in hle_d3d9_gl.h */
    static native long[] nativeStatus();
    /** {fps*10, frame ms*10, renderer 0 none/1 GLES/2 zink/3 Vulkan, fps limit}: see me1_perf() in hle_d3d9.c */
    static native long[] nativePerf();
    private static final String[] RENDERERS = { "нет вывода", "GLES", "GLES (zink)", "Vulkan" };
    private ControlsOverlay overlay;
    private TextView status, badge, fps;
    private ImageView splash;
    private boolean rumble, rumblePhone, vibrating;
    private Vibrator phoneVib;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private long t0;
    private android.content.SharedPreferences cprefs;
    private boolean polling;
    private static GameActivity current;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        L.init(this);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = w.getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            w.setAttributes(lp);
        }
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        sv.setFocusable(true); sv.setFocusableInTouchMode(true);
        FrameLayout root = new FrameLayout(this);
        root.addView(sv);
        splash = new ImageView(this);   /* loading picture until the first frame (assets/loading.jpg) */
        try (java.io.InputStream in = getAssets().open("loading.jpg")) {
            splash.setImageBitmap(BitmapFactory.decodeStream(in)); splash.setScaleType(ImageView.ScaleType.CENTER_CROP);
            splash.setBackgroundColor(Color.BLACK);
            root.addView(splash, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        } catch (Exception e) { splash = null; }
        overlay = new ControlsOverlay(this, this::mouseTouch);   /* on-screen controls (toggle in the top-left corner) */
        root.addView(overlay);
        float dp = getResources().getDisplayMetrics().density;
        status = new TextView(this);   /* not clickable: touches go through to the controls */
        status.setTextColor(Color.rgb(0xE0, 0xE6, 0xEE)); status.setTextSize(15); status.setGravity(Gravity.CENTER);
        status.setBackgroundColor(0xD0101418); status.setPadding((int) (24 * dp), (int) (16 * dp), (int) (24 * dp), (int) (16 * dp));
        status.setText(L.t("Mass Effect запускается…"));
        FrameLayout.LayoutParams sl = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                splash != null ? Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL : Gravity.CENTER);
        if (splash != null) { sl.bottomMargin = (int) (12 * dp); status.setBackgroundColor(0xB0101418); status.setTextSize(13); }
        root.addView(status, sl);
        badge = new TextView(this);
        badge.setTextColor(0xE0FFD080); badge.setTextSize(12); badge.setBackgroundColor(0x90101418);
        badge.setPadding((int) (10 * dp), (int) (4 * dp), (int) (10 * dp), (int) (4 * dp)); badge.setVisibility(View.GONE);
        FrameLayout.LayoutParams bl = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        bl.topMargin = (int) (6 * dp);
        root.addView(badge, bl);
        String[] env = getIntent().getStringArrayExtra("env");
        boolean showFps = false;
        if (env != null) for (String v : env) if ("ME1_SHOW_FPS=1".equals(v)) showFps = true;
        if (showFps) {   /* [Port] ShowFps / ME1_SHOW_FPS=1: counter in the top-right corner (the controls toggle is top-left) */
            fps = new TextView(this);
            fps.setTextColor(0xF0A0FF90); fps.setTextSize(12); fps.setBackgroundColor(0x80000000); fps.setTypeface(android.graphics.Typeface.MONOSPACE);
            fps.setPadding((int) (8 * dp), (int) (3 * dp), (int) (8 * dp), (int) (3 * dp)); fps.setVisibility(View.GONE);
            FrameLayout.LayoutParams fl = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP | Gravity.END);
            fl.topMargin = (int) (6 * dp); fl.rightMargin = (int) (6 * dp);
            root.addView(fps, fl);
        }
        setContentView(root);
        t0 = SystemClock.uptimeMillis();
        cprefs = ControlsSettings.prefs(this);
        readPadSettings();
        phoneVib = (Vibrator) getSystemService(VIBRATOR_SERVICE);
        sv.requestFocus();
        hideBars();
        current = this;
        Intent it = getIntent();
        nativeStart(it.getStringArrayExtra("args"), it.getStringArrayExtra("env"), it.getStringExtra("log"));
    }
    private void hideBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }
    @Override public void onWindowFocusChanged(boolean has) { super.onWindowFocusChanged(has); if (has) hideBars(); }
    @Override public void surfaceCreated(SurfaceHolder h) { nativeSurface(h.getSurface()); }
    @Override public void surfaceChanged(SurfaceHolder h, int format, int width, int height) { }
    @Override public void surfaceDestroyed(SurfaceHolder h) { nativeSurface(null); }

    @Override protected void onPause() {
        super.onPause(); if (overlay != null) overlay.releaseAll(); pad.releaseAll(); stopRumble();
        polling = false; ui.removeCallbacks(poll); ui.removeCallbacks(rumblePoll);
    }
    @Override protected void onResume() {
        super.onResume(); readPadSettings();
        if (!polling) { polling = true; ui.postDelayed(poll, 300); ui.postDelayed(rumblePoll, 500); }
    }
    /** profile + camera speed for the pad; called on start/resume and when the controls settings change */
    void readPadSettings() {
        if (cprefs == null) return;
        pad.xinput = cprefs.getBoolean("xinput", false);
        pad.lookScale = Math.max(2, Math.min(40, cprefs.getInt("look", 12))) / 10f;
        pad.invertY = cprefs.getBoolean("invertY", false);
        rumble = cprefs.getBoolean("rumble", true); rumblePhone = cprefs.getBoolean("rumblePhone", true);
    }

    /* ---- physical gamepads ---- */
    private static boolean isPad(InputEvent e) {
        int s = e.getSource();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }
    private static float axis(MotionEvent e, int a) { return e.getAxisValue(a); }
    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK || e.getActionMasked() != MotionEvent.ACTION_MOVE)
            return super.onGenericMotionEvent(e);
        InputDevice d = e.getDevice();
        /* right stick: Z/RZ on most pads; RX/RY when Z/RZ are triggers (range 0..1) */
        boolean zTrig = false;
        if (d != null) { InputDevice.MotionRange z = d.getMotionRange(MotionEvent.AXIS_Z, e.getSource()); zTrig = z != null && z.getMin() >= 0 && d.getMotionRange(MotionEvent.AXIS_RX, e.getSource()) != null; }
        float rx = zTrig ? axis(e, MotionEvent.AXIS_RX) : axis(e, MotionEvent.AXIS_Z), ry = zTrig ? axis(e, MotionEvent.AXIS_RY) : axis(e, MotionEvent.AXIS_RZ);
        float lt = Math.max(axis(e, MotionEvent.AXIS_LTRIGGER), axis(e, MotionEvent.AXIS_BRAKE)), rt = Math.max(axis(e, MotionEvent.AXIS_RTRIGGER), axis(e, MotionEvent.AXIS_GAS));
        if (zTrig) { lt = Math.max(lt, axis(e, MotionEvent.AXIS_Z)); rt = Math.max(rt, axis(e, MotionEvent.AXIS_RZ)); }
        pad.lastDevice = e.getDeviceId(); pad.lastInput = SystemClock.uptimeMillis();
        pad.physAxes(axis(e, MotionEvent.AXIS_X), axis(e, MotionEvent.AXIS_Y), rx, ry, lt, rt, axis(e, MotionEvent.AXIS_HAT_X), axis(e, MotionEvent.AXIS_HAT_Y));
        return true;
    }
    private boolean padKey(int code, KeyEvent e, boolean down) {
        boolean dpad = code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT;
        if (!KeyEvent.isGamepadButton(code) && !(dpad && isPad(e))) return false;
        if (down && e.getRepeatCount() > 0) return true;
        pad.lastDevice = e.getDeviceId(); pad.lastInput = SystemClock.uptimeMillis();
        return pad.physKey(code, down);
    }
    /* ---- rumble: XInputSetState -> controller vibrator, else the phone ---- */
    private final Runnable rumblePoll = new Runnable() {
        @Override public void run() {
            if (!polling) return;
            int v = 0;
            if (rumble && pad.xinput) try { v = nativePadVibration(); } catch (Throwable ignored) { }
            int amp = Math.max(v >>> 16, v & 0xFFFF) >> 8;
            if (amp > 8) vibrate(Math.min(255, amp)); else stopRumble();
            ui.postDelayed(this, 100);
        }
    };
    private Vibrator target() {
        InputDevice d = pad.lastDevice >= 0 ? InputDevice.getDevice(pad.lastDevice) : null;
        if (d != null) { Vibrator v = d.getVibrator(); if (v != null && v.hasVibrator()) return v; }
        return rumblePhone && phoneVib != null && phoneVib.hasVibrator() ? phoneVib : null;
    }
    private void vibrate(int amp) {
        Vibrator v = target(); if (v == null) return;
        try { v.vibrate(VibrationEffect.createOneShot(150, v.hasAmplitudeControl() ? amp : VibrationEffect.DEFAULT_AMPLITUDE)); vibrating = true; }
        catch (Exception ignored) { }
    }
    private void stopRumble() {
        if (!vibrating) return;
        vibrating = false;
        Vibrator v = target(); if (v != null) try { v.cancel(); } catch (Exception ignored) { }
    }

    /* start-up / shader compilation screen */
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (!polling) return;
            long[] s = null;
            try { s = nativeStatus(); } catch (Throwable ignored) { }
            if (s != null && s.length >= 10) showStatus(s);
            if (fps != null && s != null && s[0] != 0) {
                long[] p = null;
                try { p = nativePerf(); } catch (Throwable ignored) { }
                if (p != null && p.length >= 4 && (p[0] > 0 || p[1] > 0)) {
                    String r = p[2] >= 0 && p[2] < RENDERERS.length ? L.t(RENDERERS[(int) p[2]]) : "?";
                    String t = String.format(Locale.ROOT, L.t("%.1f FPS · %.1f мс · %s"), p[0] / 10.0, p[1] / 10.0, r);
                    if (p[3] > 0) t += L.t(" · лимит ") + p[3];
                    fps.setText(t); fps.setVisibility(View.VISIBLE);
                }
            }
            ui.postDelayed(this, 300);
        }
    };
    private void showStatus(long[] s) {
        boolean compiling = s[3] != 0;
        if (s[0] != 0 && splash != null) { splash.setVisibility(View.GONE); splash.setImageDrawable(null); splash = null; }
        if (s[0] == 0) {   /* no frame yet */
            long el = (SystemClock.uptimeMillis() - t0) / 1000;
            StringBuilder b = new StringBuilder();
            b.append(L.t("Mass Effect запускается… ")).append(el / 60).append(':').append(String.format(Locale.ROOT, "%02d", el % 60)).append("\n\n");
            b.append(L.t("Шейдеры D3D создано: ")).append(s[8]).append('\n');
            b.append(L.t("Компиляция HLSL: ")).append(s[1] - s[2]).append(L.t(", из кэша: ")).append(s[2]);
            if (compiling) b.append(String.format(Locale.ROOT, L.t("\nсейчас компилируется шейдер (%.1f с)"), s[4] / 1000.0));
            b.append(L.t("\nПрограммы GPU: ")).append(s[5]).append(L.t(" (из кэша ")).append(s[6]).append(")\n\n");
            b.append(compiling || s[1] > s[2]
                    ? L.t("Идёт компиляция шейдеров. Первый запуск может занять\nнесколько минут; результат сохраняется в кэш.")
                    : L.t("Загрузка данных игры…"));
            status.setText(b); status.setVisibility(View.VISIBLE);
            badge.setVisibility(View.GONE);
            return;
        }
        status.setVisibility(View.GONE);
        boolean show = cprefs.getBoolean("badge", true) && (compiling || (s[9] >= 0 && s[9] < 1500));
        if (show) badge.setText(compiling ? String.format(Locale.ROOT, L.t("⚙ компиляция шейдера… %.1f с"), s[4] / 1000.0) : L.t("⚙ сборка шейдеров (GL: ") + s[5] + ")");
        badge.setVisibility(show ? View.VISIBLE : View.GONE);
    }
    @Override public boolean onTouchEvent(MotionEvent e) { mouseTouch(e); return true; }
    /** the screen as the mouse (controls hidden) */
    void mouseTouch(MotionEvent e) {
        switch (e.getActionMasked()) {
        case MotionEvent.ACTION_DOWN: nativeTouch(0, e.getX(0), e.getY(0)); break;
        case MotionEvent.ACTION_MOVE: nativeTouch(2, e.getX(0), e.getY(0)); break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL: nativeTouch(1, e.getX(0), e.getY(0)); break;
        case MotionEvent.ACTION_POINTER_DOWN: if (e.getActionIndex() == 1) nativeTouch(3, e.getX(0), e.getY(0)); break;
        case MotionEvent.ACTION_POINTER_UP: if (e.getActionIndex() == 1) nativeTouch(4, e.getX(0), e.getY(0)); break;
        }
    }
    private static boolean passThrough(int code) {
        return code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_VOLUME_MUTE
                || code == KeyEvent.KEYCODE_POWER || code == KeyEvent.KEYCODE_HOME;
    }
    @Override public boolean onKeyDown(int code, KeyEvent e) {
        if (passThrough(code)) return super.onKeyDown(code, e);
        if (padKey(code, e, true)) return true;
        if (e.getRepeatCount() == 0 || code != KeyEvent.KEYCODE_BACK) nativeKey(code, 1, e.getUnicodeChar());
        return true;
    }
    @Override public boolean onKeyUp(int code, KeyEvent e) {
        if (passThrough(code)) return super.onKeyUp(code, e);
        if (padKey(code, e, false)) return true;
        nativeKey(code, 0, 0);
        return true;
    }
    @Override public void onBackPressed() { }   /* Back is Escape (handled in onKeyDown/onKeyUp) */

    /** called from the game thread when MassEffect.exe returns (the process exits right after) */
    static void onGameExit(int code) {
        final GameActivity a = current;
        if (a != null) a.runOnUiThread(a::finish);
    }

    /** v0.10.2: one-time events from the renderer (Vulkan VK_ERROR_DEVICE_LOST on malisx2) — a readable Toast */
    static void onDeviceLost(String msg) {
        final GameActivity a = current;
        if (a == null || msg == null) return;
        a.runOnUiThread(() -> Toast.makeText(a.getApplicationContext(), msg, Toast.LENGTH_LONG).show());
    }

    /** v0.10.10: the GPU driver dies serially — native exits with code 77; schedule a fresh process via AlarmManager
     * (same intent: game folder, env, log). Shader caches are warm, so the relaunch is quick. */
    static void onGameRestart() {
        final GameActivity a = current;
        if (a == null) return;
        a.runOnUiThread(() -> {
            try {
                android.app.AlarmManager am = (android.app.AlarmManager) a.getSystemService(ALARM_SERVICE);
                android.content.Intent again = new android.content.Intent(a.getIntent());   /* v0.10.13: compile fix (no cloneIntent / PendingIntent.FLAG_ACTIVITY_NEW_TASK) */
                again.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                android.app.PendingIntent pi = android.app.PendingIntent.getActivity(a, 9001, again,
                    android.app.PendingIntent.FLAG_ONE_SHOT | android.app.PendingIntent.FLAG_IMMUTABLE);
                if (am != null) am.setExact(android.app.AlarmManager.RTC, System.currentTimeMillis() + 1200, pi);
                Toast.makeText(a.getApplicationContext(), L.t("Видеодрайвер умирает серийно — игра перезапускается (кэши шейдеров тёплые)"), Toast.LENGTH_LONG).show();
            } catch (Throwable ignored) { }
            a.finish();
        });
    }
}
