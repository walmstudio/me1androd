package com.me1port.launcher;

import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;

/** Gamepad state -> game (v0.9.7). Two sources are merged: physical controllers (GameActivity: onGenericMotionEvent /
 * onKeyDown) and the on-screen Xbox pad (ControlsOverlay in the "xinput" profile).
 *  - profile "xinput": the merged state is the game's XInput pad 0 (GameActivity.nativePad). The vanilla game has no
 *    bindings for XboxTypeS_* keys - a controller mod such as ME1Controller (BIOInput.ini) is needed;
 *  - profile "kbm": a physical pad emulates the PC keyboard and mouse (works with the unmodified game):
 *    left stick = W/A/S/D, right stick = camera (mouse), RT = left mouse button (fire), LT = right button (zoom),
 *    A = E (use), B = Q, X = Space (squad/powers menu), Y = F, LB = R, RB = Shift (sprint), L3 = Ctrl, R3 = J,
 *    Start = Esc, Back = M (map), D-pad = arrow keys. */
final class Pad {
    static final int UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, START = 0x10, BACK = 0x20, LTHUMB = 0x40, RTHUMB = 0x80,
            LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    boolean xinput;           /* profile */
    float lookScale = 1.2f;   /* kbm: right stick camera speed (ControlsSettings "look") */
    boolean invertY;
    /* physical: buttons from key events + d-pad from the HAT axes */
    int pKeys, pHat; float pLX, pLY, pRX, pRY, pLT, pRT;
    /* on-screen */
    int vBtn; float vLX, vLY, vRX, vRY, vLT, vRT;
    /* last sent */
    private int sBtn = -1, sLT, sRT, sLX, sLY, sRX, sRY;
    /* kbm emulation state */
    private int kBtn; private boolean kw, ka, ks, kd, lmb, rmb;
    private float kRX, kRY;
    private final Handler h = new Handler(Looper.getMainLooper());
    private boolean ticking;
    long lastInput;           /* uptime of the last physical pad input (rumble target) */
    int lastDevice = -1;

    /** Android gamepad key -> XInput button bit (0 = not a pad button); triggers are handled separately */
    static int bit(int code) {
        switch (code) {
        case KeyEvent.KEYCODE_BUTTON_A: return A;
        case KeyEvent.KEYCODE_BUTTON_B: return B;
        case KeyEvent.KEYCODE_BUTTON_X: return X;
        case KeyEvent.KEYCODE_BUTTON_Y: return Y;
        case KeyEvent.KEYCODE_BUTTON_L1: return LB;
        case KeyEvent.KEYCODE_BUTTON_R1: return RB;
        case KeyEvent.KEYCODE_BUTTON_THUMBL: return LTHUMB;
        case KeyEvent.KEYCODE_BUTTON_THUMBR: return RTHUMB;
        case KeyEvent.KEYCODE_BUTTON_START: return START;
        case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_BUTTON_MODE: return BACK;
        case KeyEvent.KEYCODE_DPAD_UP: return UP;
        case KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
        case KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
        case KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
        default: return 0;
        }
    }
    /** physical button; returns false if the key is not a pad button */
    boolean physKey(int code, boolean down) {
        if (code == KeyEvent.KEYCODE_BUTTON_L2) { pLT = down ? 1 : 0; flush(); return true; }
        if (code == KeyEvent.KEYCODE_BUTTON_R2) { pRT = down ? 1 : 0; flush(); return true; }
        int b = bit(code); if (b == 0) return false;
        pKeys = down ? pKeys | b : pKeys & ~b; flush(); return true;
    }
    void physAxes(float lx, float ly, float rx, float ry, float lt, float rt, float hx, float hy) {
        pLX = lx; pLY = ly; pRX = rx; pRY = ry; pLT = lt; pRT = rt;
        pHat = (hy < -0.5f ? UP : 0) | (hy > 0.5f ? DOWN : 0) | (hx < -0.5f ? LEFT : 0) | (hx > 0.5f ? RIGHT : 0);
        flush();
    }
    void virtButton(int b, boolean down) { vBtn = down ? vBtn | b : vBtn & ~b; flush(); }
    void virtTrigger(int which, boolean down) { if (which == 1) vLT = down ? 1 : 0; else vRT = down ? 1 : 0; flush(); }
    void virtLeft(float x, float y) { vLX = x; vLY = y; flush(); }
    void virtRight(float x, float y) { vRX = x; vRY = y; flush(); }
    void releaseAll() { pKeys = pHat = vBtn = 0; pLX = pLY = pRX = pRY = pLT = pRT = 0; vLX = vLY = vRX = vRY = vLT = vRT = 0; flush(); }

    private static float pick(float a, float b) { return Math.abs(a) >= Math.abs(b) ? a : b; }
    private static int s16(float v) { v = Math.max(-1f, Math.min(1f, v)); return Math.round(v * 32767f); }
    private static int u8(float v) { v = Math.max(0f, Math.min(1f, v)); return Math.round(v * 255f); }
    /** merged state -> game (XInput) or -> keyboard/mouse emulation */
    void flush() {
        int btn = pKeys | pHat | vBtn;
        float lx = pick(pLX, vLX), ly = pick(pLY, vLY), rx = pick(pRX, vRX), ry = pick(pRY, vRY), lt = Math.max(pLT, vLT), rt = Math.max(pRT, vRT);
        if (xinput) {
            int LX = s16(lx), LY = s16(-ly), RX = s16(rx), RY = s16(-ry), LT = u8(lt), RT = u8(rt);   /* XInput: Y up = positive */
            if (btn == sBtn && LX == sLX && LY == sLY && RX == sRX && RY == sRY && LT == sLT && RT == sRT) return;
            sBtn = btn; sLX = LX; sLY = LY; sRX = RX; sRY = RY; sLT = LT; sRT = RT;
            try { GameActivity.nativePad(btn, LT, RT, LX, LY, RX, RY); } catch (Throwable ignored) { }
            return;
        }
        emulate(btn, lx, ly, rx, ry, lt, rt);
    }
    /* ---- keyboard + mouse emulation (profile kbm) ---- */
    private static final int[][] MAP = {   /* pad bit -> Android keycode (sent as a PC key by nativeKey) */
        { A, KeyEvent.KEYCODE_E }, { B, KeyEvent.KEYCODE_Q }, { X, KeyEvent.KEYCODE_SPACE }, { Y, KeyEvent.KEYCODE_F },
        { LB, KeyEvent.KEYCODE_R }, { RB, KeyEvent.KEYCODE_SHIFT_LEFT }, { LTHUMB, KeyEvent.KEYCODE_CTRL_LEFT }, { RTHUMB, KeyEvent.KEYCODE_J },
        { START, KeyEvent.KEYCODE_ESCAPE }, { BACK, KeyEvent.KEYCODE_M },
        { UP, KeyEvent.KEYCODE_DPAD_UP }, { DOWN, KeyEvent.KEYCODE_DPAD_DOWN }, { LEFT, KeyEvent.KEYCODE_DPAD_LEFT }, { RIGHT, KeyEvent.KEYCODE_DPAD_RIGHT } };
    private void emulate(int btn, float lx, float ly, float rx, float ry, float lt, float rt) {
        int ch = btn ^ kBtn;
        if (ch != 0) for (int[] m : MAP) if ((ch & m[0]) != 0) GameActivity.nativeKey(m[1], (btn & m[0]) != 0 ? 1 : 0, 0);
        kBtn = btn;
        float t = 0.45f;
        boolean w = ly < -t && -ly > Math.abs(lx) * 0.42f, s = ly > t && ly > Math.abs(lx) * 0.42f;
        boolean a = lx < -t && -lx > Math.abs(ly) * 0.42f, d = lx > t && lx > Math.abs(ly) * 0.42f;
        if (w != kw) { kw = w; GameActivity.nativeKey(KeyEvent.KEYCODE_W, w ? 1 : 0, 0); }
        if (a != ka) { ka = a; GameActivity.nativeKey(KeyEvent.KEYCODE_A, a ? 1 : 0, 0); }
        if (s != ks) { ks = s; GameActivity.nativeKey(KeyEvent.KEYCODE_S, s ? 1 : 0, 0); }
        if (d != kd) { kd = d; GameActivity.nativeKey(KeyEvent.KEYCODE_D, d ? 1 : 0, 0); }
        boolean l = rt > 0.5f, r = lt > 0.5f;
        if (l != lmb) { lmb = l; GameActivity.nativeMouseButton(0, l ? 1 : 0); }
        if (r != rmb) { rmb = r; GameActivity.nativeMouseButton(1, r ? 1 : 0); }
        kRX = Math.abs(rx) > 0.15f ? rx : 0; kRY = Math.abs(ry) > 0.15f ? ry : 0;
        if ((kRX != 0 || kRY != 0) && !ticking) { ticking = true; h.post(tick); }
    }
    /** right stick -> relative mouse motion, 60 times a second while the stick is deflected (quadratic response) */
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (kRX == 0 && kRY == 0) { ticking = false; return; }
            float k = 16f * lookScale;
            float dx = Math.signum(kRX) * kRX * kRX * k, dy = Math.signum(kRY) * kRY * kRY * k;
            GameActivity.nativeMouseRel(dx, invertY ? -dy : dy);
            h.postDelayed(this, 16);
        }
    };
}
