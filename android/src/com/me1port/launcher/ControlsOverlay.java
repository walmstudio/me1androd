package com.me1port.launcher;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.SystemClock;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Locale;

/** On-screen controls drawn over the game. A small translucent button in the top-left corner shows/hides them;
 * a long press on it opens the settings (ControlsSettings: opacity, size, camera sensitivity, mirrored layout, which
 * buttons are shown) and the layout editor (drag the buttons and the stick anywhere; positions are saved).
 * Hidden: the screen is the mouse (one finger = left button, second finger = right button).
 * Shown: left stick = W/A/S/D, buttons send PC keys / mouse buttons, a drag on the free area turns the camera
 * (relative mouse motion), a short tap on the free area is a left click at that point (menus, dialogue wheel).
 * v0.9.7, profile "xinput" (ControlsSettings "xinput"): an Xbox pad instead - analog left stick, A/B/X/Y, LB/RB, LT/RT,
 * Back/Start, d-pad, L3/R3; a drag on the free area is a floating right stick (camera). It feeds GameActivity.pad
 * (XInput pad 0 of the game; needs a controller mod such as ME1Controller).
 * v1.1.3, layout "skyrim" (ControlsSettings "layout"): the on-screen pad of the Skyrim port, re-implemented here - coloured
 * A/B/X/Y diamond bottom-right, LB/RB (+ LT/RT) at the sides, Back/Start on top, a fixed stick that any finger on its half
 * of the screen drives (no need to hit it), fingers slide from button to button, a ⌨ key for the phone keyboard (stays
 * visible when the controls are hidden). Sizes are fractions of the screen height. Buttons go through Pad: the game's
 * XInput pad with the "xinput" profile, otherwise Pad's keyboard + mouse emulation; the other half of the screen is the
 * camera (floating right stick / mouse) and a short tap there clicks. Positions (s_<id>) and hidden buttons are its own.
 * v1.2, layout "custom": Winlator-style free elements from a profile (CustomControls, CtlProfile: .icp import / export);
 * the free area stays the camera / tap-to-click, the layout editor is CustomControls' own (toolbar, drag, pinch). */
public class ControlsOverlay extends View {
    interface Mouse { void touch(MotionEvent e); }

    static final class Btn {
        final String label; final int key; final int mouse; final boolean toggle;   /* key: Android keycode, mouse: 0 left / 1 right / -1 */
        int padBit, trig, color;   /* xinput profile: XINPUT_GAMEPAD bit, trigger (1 LT, 2 RT), tint */
        String id; boolean kbd;    /* skyrim layout: id in prefs (s_<id>, hidden "s:<id>"); kbd: the phone-keyboard key */
        float x, y, r; int pointer = -1; boolean latched, hidden;
        Btn(String label, int key, int mouse, boolean toggle) { this.label = label; this.key = key; this.mouse = mouse; this.toggle = toggle; }
        static Btn pad(String label, int bit, int trig, int color) { Btn b = new Btn(label, 0, -1, false); b.padBit = bit; b.trig = trig; b.color = color; return b; }
        boolean isPad() { return padBit != 0 || trig != 0; }
        boolean down() { return pointer >= 0 || latched; }
    }

    private final Mouse mouse;
    private final SharedPreferences prefs;
    private final float dp;
    private boolean shown, editing;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<Btn> kbmBtns = new ArrayList<>(), padBtns = new ArrayList<>(), skyBtns = new ArrayList<>();
    private ArrayList<Btn> btns = kbmBtns;
    private boolean padMode, skyMode, customMode;
    private final CustomControls cc;
    private Btn kbdBtn; private int kbdPtr = -1;   /* skyrim layout: ⌨ (and the finger on it while the controls are hidden) */
    private float lcx, lcy;   /* xinput: centre of the floating right stick */
    private final Btn toggleBtn = new Btn("⊕", 0, -1, false);
    /* stick */
    private float sx, sy, sr, kx, ky; private int stickPtr = -1; private boolean kw, ka, ks, kd;
    /* look / tap area */
    private int lookPtr = -1; private float lx, ly, ldx, ldy; private long lt; private boolean lmoved;
    private int togglePtr = -1; private boolean longFired;
    private final Runnable longPress = () -> { if (togglePtr >= 0) { longFired = true; haptic(HapticFeedbackConstants.LONG_PRESS); openSettings(); } };
    /* settings */
    private float alpha = 1f, size = 1f, stickSize = 1f, lookScale = 1.2f; private boolean invertY, tapClick = true, vibrate = true, mirror;
    /* layout editor: the dragged button (null + editStick: the stick) */
    private int editPtr = -1; private Btn editBtn; private boolean editStick; private float edx, edy;

    public ControlsOverlay(Context c, Mouse mouse) {
        super(c);
        this.mouse = mouse;
        prefs = ControlsSettings.prefs(c);
        shown = prefs.getBoolean("shown", false);
        dp = c.getResources().getDisplayMetrics().density;
        cc = new CustomControls(new CustomControls.Host() {
            @Override public void tap() { haptic(HapticFeedbackConstants.KEYBOARD_TAP); }
            @Override public void keyboard() { ControlsOverlay.this.keyboard(); }
            @Override public boolean keyboardOn() { return ControlsOverlay.this.keyboardOn(); }
            @Override public void editElement(final CtlProfile.El el) {
                Context ctx = getContext(); if (!(ctx instanceof Activity)) return;
                CtlDialogs.element((Activity) ctx, el, () -> { cc.place(el); cc.save(); ControlsOverlay.this.invalidate(); },
                        () -> { cc.remove(el); ControlsOverlay.this.invalidate(); });
            }
            @Override public void editDone() { finishEdit(); }
            @Override public void invalidate() { ControlsOverlay.this.invalidate(); }
            @Override public Context ctx() { return getContext(); }
        }, dp);
        stroke.setStyle(Paint.Style.STROKE); stroke.setStrokeWidth(2 * dp);
        text.setTextAlign(Paint.Align.CENTER); text.setFakeBoldText(true);
        ArrayList<Btn> btns = kbmBtns;
        btns.add(new Btn("ЛКМ", 0, 0, false));
        btns.add(new Btn("ПКМ", 0, 1, false));
        btns.add(new Btn("E", KeyEvent.KEYCODE_E, -1, false));
        btns.add(new Btn("Пробел", KeyEvent.KEYCODE_SPACE, -1, false));
        btns.add(new Btn("Shift", KeyEvent.KEYCODE_SHIFT_LEFT, -1, true));
        btns.add(new Btn("Q", KeyEvent.KEYCODE_Q, -1, false));
        btns.add(new Btn("G", KeyEvent.KEYCODE_G, -1, false));
        btns.add(new Btn("Tab", KeyEvent.KEYCODE_TAB, -1, false));
        btns.add(new Btn("1", KeyEvent.KEYCODE_1, -1, false));
        btns.add(new Btn("2", KeyEvent.KEYCODE_2, -1, false));
        btns.add(new Btn("3", KeyEvent.KEYCODE_3, -1, false));
        btns.add(new Btn("4", KeyEvent.KEYCODE_4, -1, false));
        btns.add(new Btn("M", KeyEvent.KEYCODE_M, -1, false));
        btns.add(new Btn("I", KeyEvent.KEYCODE_I, -1, false));
        btns.add(new Btn("J", KeyEvent.KEYCODE_J, -1, false));
        btns.add(new Btn("Esc", KeyEvent.KEYCODE_ESCAPE, -1, false));
        padBtns.add(Btn.pad("A", Pad.A, 0, 0xFF3FAE49)); padBtns.add(Btn.pad("B", Pad.B, 0, 0xFFD8443A));
        padBtns.add(Btn.pad("X", Pad.X, 0, 0xFF3A7BD8)); padBtns.add(Btn.pad("Y", Pad.Y, 0, 0xFFE0B020));
        padBtns.add(Btn.pad("LB", Pad.LB, 0, 0)); padBtns.add(Btn.pad("RB", Pad.RB, 0, 0));
        padBtns.add(Btn.pad("LT", 0, 1, 0)); padBtns.add(Btn.pad("RT", 0, 2, 0));
        padBtns.add(Btn.pad("Back", Pad.BACK, 0, 0)); padBtns.add(Btn.pad("Start", Pad.START, 0, 0));
        padBtns.add(Btn.pad("↑", Pad.UP, 0, 0)); padBtns.add(Btn.pad("↓", Pad.DOWN, 0, 0));
        padBtns.add(Btn.pad("←", Pad.LEFT, 0, 0)); padBtns.add(Btn.pad("→", Pad.RIGHT, 0, 0));
        padBtns.add(Btn.pad("L3", Pad.LTHUMB, 0, 0)); padBtns.add(Btn.pad("R3", Pad.RTHUMB, 0, 0));
        String[] sid = ControlsSettings.SKY_BUTTONS;
        int[] sbit = { Pad.A, Pad.B, Pad.X, Pad.Y, Pad.LB, Pad.RB, 0, 0, Pad.BACK, Pad.START };
        int[] scol = { 0xFF3FAE49, 0xFFD8443A, 0xFF3A7BD8, 0xFFE0B020, 0xFFAEB4BC, 0xFFAEB4BC, 0xFF8E959E, 0xFF8E959E, 0xFFD4D8DD, 0xFFF4F6F8 };
        for (int i = 0; i < sbit.length; i++) { Btn b = Btn.pad(sid[i], sbit[i], i == 6 ? 1 : i == 7 ? 2 : 0, scol[i]); b.id = sid[i]; skyBtns.add(b); }
        kbdBtn = new Btn("⌨", 0, -1, false); kbdBtn.id = "kbd"; kbdBtn.kbd = true; kbdBtn.color = 0xFFE2E6EA; skyBtns.add(kbdBtn);
        readSettings();
    }

    /** re-read ControlsSettings (called by the settings dialog after every change) */
    void reload() {
        releaseAll(); shown = prefs.getBoolean("shown", shown); readSettings(); layout(getWidth(), getHeight()); invalidate();
        if (getContext() instanceof GameActivity) ((GameActivity) getContext()).readPadSettings();
    }
    private void readSettings() {
        alpha = clamp(prefs.getInt("alpha", 100), 20, 100) / 100f;
        size = clamp(prefs.getInt("size", 100), 50, 200) / 100f;
        stickSize = clamp(prefs.getInt("stick", 100), 50, 200) / 100f;
        lookScale = clamp(prefs.getInt("look", 12), 2, 40) / 10f;
        invertY = prefs.getBoolean("invertY", false); tapClick = prefs.getBoolean("tapClick", true);
        vibrate = prefs.getBoolean("haptic", true); mirror = prefs.getBoolean("mirror", false);
        padMode = prefs.getBoolean("xinput", false); skyMode = ControlsSettings.sky(prefs); customMode = ControlsSettings.custom(prefs);
        cc.size = size; cc.mirror = mirror; cc.look = lookScale; cc.invertY = invertY; cc.tapClick = tapClick; cc.alpha = alpha;
        if (customMode) cc.load();
        btns = skyMode ? skyBtns : padMode ? padBtns : kbmBtns;
        HashSet<String> hid = ControlsSettings.hidden(prefs);
        for (Btn b : kbmBtns) b.hidden = hid.contains(b.label);
        for (Btn b : padBtns) b.hidden = hid.contains(b.label);
        for (Btn b : skyBtns) b.hidden = hid.contains("s:" + b.id);
    }
    private static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) { layout(w, h); }
    private Btn find(String l) { for (Btn b : btns) if (b.label.equals(l)) return b; return null; }
    private void place(String l, float x, float y, float r) { Btn b = find(l); b.x = x; b.y = y; b.r = r; }
    private void layout(int w, int h) {
        if (w <= 0 || h <= 0) return;
        float m = 14 * dp, big = 34 * dp * size, mid = 26 * dp * size, sm = 19 * dp * size;
        toggleBtn.x = m + 18 * dp; toggleBtn.y = m + 18 * dp; toggleBtn.r = 18 * dp;
        if (customMode) { if (mirror) toggleBtn.x = w - toggleBtn.x; cc.layout(w, h); return; }
        if (skyMode) { layoutSky(w, h, m); return; }
        if (padMode) { layoutPad(w, h, m, mid, sm); return; }
        place("Esc", toggleBtn.x + 28 * dp + sm, toggleBtn.y, sm);
        sr = Math.min(64 * dp, h * 0.22f) * stickSize; sx = m + sr + 8 * dp; sy = h - m - sr - 8 * dp;
        place("Shift", sx + sr * 0.15f, sy - sr - 14 * dp - mid, mid);
        place("Tab", sx + sr * 0.15f + mid + sm + 10 * dp, sy - sr - 14 * dp - mid, sm);
        /* right side, from the bottom-right corner */
        float rx = w - m - big - 6 * dp, ry = h - m - big - 6 * dp;
        place("ЛКМ", rx, ry, big);
        place("ПКМ", rx - big - mid - 14 * dp, ry + (big - mid) * 0.7f, mid);
        place("E", rx - 6 * dp * size, ry - big - mid - 12 * dp, mid);
        place("Пробел", rx - big - mid - 22 * dp, ry - big - mid - 6 * dp, mid);
        place("Q", rx - big - mid - 2 * sm - 34 * dp, ry - 2 * big - mid - 6 * dp, sm);
        place("G", rx - big - sm - 4 * dp, ry - 2 * big - mid - 22 * dp, sm);
        float tx = w - m - sm - 4 * dp, ty = m + sm + 4 * dp, step = 2 * sm + 8 * dp;
        String[] row1 = { "4", "3", "2", "1" }, row2 = { "J", "I", "M" };
        for (int i = 0; i < row1.length; i++) place(row1[i], tx - i * step, ty, sm);
        for (int i = 0; i < row2.length; i++) place(row2[i], tx - i * step, ty + step, sm);
        if (mirror) {   /* stick on the right, buttons on the left, ⊕ in the top-right corner */
            for (Btn b : btns) b.x = w - b.x;
            toggleBtn.x = w - toggleBtn.x; sx = w - sx;
        }
        /* positions from the layout editor */
        for (Btn b : btns) { float[] p = pos("p_" + b.label, w, h); if (p != null) { b.x = p[0]; b.y = p[1]; } }
        float[] p = pos("p_stick", w, h); if (p != null) { sx = p[0]; sy = p[1]; }
        kx = sx; ky = sy;
    }
    /** Xbox pad: stick + d-pad bottom-left, A/B/X/Y bottom-right, bumpers/triggers at the sides, Back/Start top centre */
    private void layoutPad(int w, int h, float m, float mid, float sm) {
        btns = padBtns;
        sr = Math.min(60 * dp, h * 0.2f) * stickSize; sx = m + sr + 8 * dp; sy = h - m - sr - 8 * dp;
        float g = 2.15f * mid, cx = w - m - g - mid - 4 * dp, cy = h - m - g - mid - 4 * dp;
        place("A", cx, cy + g, mid); place("B", cx + g, cy, mid); place("X", cx - g, cy, mid); place("Y", cx, cy - g, mid);
        float ty = Math.max(toggleBtn.y + 30 * dp + mid, cy - g - 2 * mid - 24 * dp);
        place("LT", m + mid + 4 * dp, ty, mid); place("LB", m + 3 * mid + 18 * dp, ty, mid);
        place("RT", w - m - mid - 4 * dp, ty, mid); place("RB", w - m - 3 * mid - 18 * dp, ty, mid);
        place("Back", w / 2f - 46 * dp, m + sm + 4 * dp, sm); place("Start", w / 2f + 46 * dp, m + sm + 4 * dp, sm);
        float dg = 2.1f * sm, dx = sx + sr + dg + sm + 26 * dp, dy = h - m - dg - sm - 4 * dp;
        place("↑", dx, dy - dg, sm); place("↓", dx, dy + dg, sm); place("←", dx - dg, dy, sm); place("→", dx + dg, dy, sm);
        place("L3", sx + sr + sm * 0.6f, sy - sr - sm * 0.4f, sm); place("R3", cx - g - mid - sm - 18 * dp, cy + g, sm);
        if (mirror) { for (Btn b : btns) b.x = w - b.x; toggleBtn.x = w - toggleBtn.x; sx = w - sx; }
        for (Btn b : btns) { float[] p = pos("p_" + b.label, w, h); if (p != null) { b.x = p[0]; b.y = p[1]; } }
        float[] p = pos("p_padstick", w, h); if (p != null) { sx = p[0]; sy = p[1]; }
        kx = sx; ky = sy;
    }
    /** "skyrim" layout: everything in fractions of the screen height (u), so it looks the same on any aspect ratio */
    private void layoutSky(int w, int h, float m) {
        float u = h, s = size;
        float r = 0.072f * u * s, g = 0.125f * u * s, cx = w - m - g - r, cy = h - m - g - r;   /* A/B/X/Y diamond */
        place("A", cx, cy + g, r); place("B", cx + g, cy, r); place("X", cx - g, cy, r); place("Y", cx, cy - g, r);
        float rb = 0.06f * u * s, rt = 0.055f * u * s, by = Math.max(0.30f * u, toggleBtn.y + toggleBtn.r + rb + 0.04f * u);
        float bx = m + rb + 0.04f * u, tx = bx + rb + rt + 0.04f * u;   /* bumpers at the edges, triggers next to them */
        place("LB", bx, by, rb); place("RB", w - bx, by, rb); place("LT", tx, by, rt); place("RT", w - tx, by, rt);
        float rs = 0.045f * u * s, ky = Math.max(toggleBtn.y, m + rs), kx = Math.max(0.2f * u, toggleBtn.x + toggleBtn.r + rs + 12 * dp);
        place("⌨", kx, ky, rs);
        float sbx = Math.max(0.36f * u, kx + 2 * rs + 0.03f * u);
        place("Back", sbx, m + rs, rs); place("Start", w - sbx, m + rs, rs);
        sr = 0.17f * u * stickSize; sx = m + sr + 0.05f * u; sy = h - m - sr - 0.07f * u;
        if (mirror) { for (Btn b : btns) b.x = w - b.x; toggleBtn.x = w - toggleBtn.x; sx = w - sx; }
        for (Btn b : btns) { float[] p = pos("s_" + b.id, w, h); if (p != null) { b.x = p[0]; b.y = p[1]; } }
        float[] p = pos("s_stick", w, h); if (p != null) { sx = p[0]; sy = p[1]; }
        kx = sx; ky = sy;
    }
    private float[] pos(String key, int w, int h) {
        String v = prefs.getString(key, null); if (v == null) return null;
        try { String[] a = v.split(","); return new float[] { Float.parseFloat(a[0]) * w, Float.parseFloat(a[1]) * h }; }
        catch (Exception e) { return null; }
    }
    private void savePos(String key, float x, float y) {
        int w = Math.max(1, getWidth()), h = Math.max(1, getHeight());
        prefs.edit().putString(key, String.format(Locale.ROOT, "%.4f,%.4f", x / w, y / h)).apply();
    }

    private static boolean hit(Btn b, float x, float y, float extra) { float dx = x - b.x, dy = y - b.y; return dx * dx + dy * dy <= (b.r + extra) * (b.r + extra); }
    private void haptic(int kind) { if (vibrate) performHapticFeedback(kind); }
    private void openSettings() {
        Context c = getContext();
        if (!(c instanceof Activity)) return;
        releaseAll();
        ControlsSettings.show((Activity) c, this::reload, this::startEdit);
    }
    void startEdit() { releaseAll(); editing = true; shown = true; cc.sel = null; prefs.edit().putBoolean("shown", true).apply(); invalidate(); }
    private void finishEdit() {
        editing = false; editPtr = -1;
        if (customMode) { cc.sel = null; cc.save(); }
        android.widget.Toast.makeText(getContext(), L.t("Расположение сохранено"), android.widget.Toast.LENGTH_SHORT).show();
        invalidate();
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int act = e.getActionMasked(), idx = e.getActionIndex(), id = e.getPointerId(idx);
        float x = e.getX(idx), y = e.getY(idx);
        if (act == MotionEvent.ACTION_DOWN || act == MotionEvent.ACTION_POINTER_DOWN) {
            if (togglePtr < 0 && hit(toggleBtn, x, y, 10 * dp)) {
                togglePtr = id; longFired = false; postDelayed(longPress, 600); invalidate(); return true;
            }
        } else if ((act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_POINTER_UP || act == MotionEvent.ACTION_CANCEL) && id == togglePtr) {
            togglePtr = -1; removeCallbacks(longPress);
            if (act != MotionEvent.ACTION_CANCEL && !longFired && hit(toggleBtn, x, y, 24 * dp)) {
                if (editing) finishEdit();
                else setShown(!shown);
            }
            invalidate(); return true;
        }
        if (editing) { if (customMode) cc.edit(act, id, x, y, e); else edit(act, id, x, y, e); invalidate(); return true; }
        if (!shown && skyMode && !kbdBtn.hidden) {   /* ⌨ works with the controls hidden too */
            if ((act == MotionEvent.ACTION_DOWN || act == MotionEvent.ACTION_POINTER_DOWN) && kbdPtr < 0 && hit(kbdBtn, x, y, kbdBtn.r * 0.3f)) {
                kbdPtr = id; haptic(HapticFeedbackConstants.KEYBOARD_TAP); keyboard(); invalidate(); return true;
            }
            if ((act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_POINTER_UP || act == MotionEvent.ACTION_CANCEL) && id == kbdPtr) { kbdPtr = -1; invalidate(); return true; }
            if (kbdPtr >= 0 && e.getPointerCount() == 1) return true;
        }
        if (!shown) {
            if (togglePtr >= 0 && e.getPointerCount() == 1) return true;   /* only the toggle finger */
            mouse.touch(e); return true;
        }
        switch (act) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: down(id, x, y); break;
        case MotionEvent.ACTION_MOVE:
            for (int i = 0; i < e.getPointerCount(); i++) move(e.getPointerId(i), e.getX(i), e.getY(i));
            break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: up(id, x, y, false); break;
        case MotionEvent.ACTION_CANCEL: releaseAll(); break;
        }
        invalidate();
        return true;
    }
    /** layout editor: drag a button or the stick */
    private void edit(int act, int id, float x, float y, MotionEvent e) {
        if (act == MotionEvent.ACTION_DOWN || act == MotionEvent.ACTION_POINTER_DOWN) {
            if (editPtr >= 0) return;
            for (Btn b : btns) if (!b.hidden && hit(b, x, y, 6 * dp)) { editPtr = id; editBtn = b; editStick = false; edx = b.x - x; edy = b.y - y; haptic(HapticFeedbackConstants.KEYBOARD_TAP); return; }
            float dx = x - sx, dy = y - sy;
            if (dx * dx + dy * dy <= sr * sr) { editPtr = id; editBtn = null; editStick = true; edx = sx - x; edy = sy - y; haptic(HapticFeedbackConstants.KEYBOARD_TAP); }
        } else if (act == MotionEvent.ACTION_MOVE && editPtr >= 0) {
            int i = e.findPointerIndex(editPtr); if (i < 0) return;
            float nx = Math.max(0, Math.min(getWidth(), e.getX(i) + edx)), ny = Math.max(0, Math.min(getHeight(), e.getY(i) + edy));
            if (editBtn != null) { editBtn.x = nx; editBtn.y = ny; } else if (editStick) { sx = nx; sy = ny; kx = sx; ky = sy; }
        } else if ((act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_POINTER_UP || act == MotionEvent.ACTION_CANCEL) && id == editPtr) {
            if (editBtn != null) savePos(skyMode ? "s_" + editBtn.id : "p_" + editBtn.label, editBtn.x, editBtn.y);
            else if (editStick) savePos(skyMode ? "s_stick" : padMode ? "p_padstick" : "p_stick", sx, sy);
            editPtr = -1; editBtn = null; editStick = false;
        }
    }
    private void down(int id, float x, float y) {
        if (customMode) { if (cc.down(id, x, y)) return; }
        else {
            for (Btn b : btns) if (!b.hidden && b.pointer < 0 && hit(b, x, y, skyMode ? b.r * 0.3f : 6 * dp)) {
                b.pointer = id;
                haptic(HapticFeedbackConstants.KEYBOARD_TAP);
                if (b.kbd) keyboard();
                else if (b.toggle) { b.latched = !b.latched; send(b, b.latched); }
                else send(b, true);
                return;
            }
            float dx = x - sx, dy = y - sy;
            boolean stickZone = skyMode ? (x < getWidth() / 2f) == (sx < getWidth() / 2f)   /* skyrim: the stick's whole half of the screen */
                    : dx * dx + dy * dy <= (sr * 1.6f) * (sr * 1.6f);
            if (stickPtr < 0 && stickZone) { stickPtr = id; stick(x, y); return; }
        }
        if (lookPtr < 0) { lookPtr = id; lx = x; ly = y; lcx = x; lcy = y; ldx = 0; ldy = 0; lt = SystemClock.uptimeMillis(); lmoved = false; }
    }
    private void move(int id, float x, float y) {
        if (customMode && cc.move(id, x, y)) return;
        if (id == stickPtr) { stick(x, y); return; }
        if (skyMode) for (Btn b : btns) if (b.pointer == id) {   /* skyrim: a finger slides from button to button */
            if (b.kbd) return;
            for (Btn o : btns) if (o != b && !o.hidden && !o.kbd && o.pointer < 0 && hit(o, x, y, 0)) {
                send(b, false); b.pointer = -1; o.pointer = id; send(o, true); haptic(HapticFeedbackConstants.KEYBOARD_TAP); break;
            }
            return;
        }
        if (id == lookPtr) {
            float dx = x - lx, dy = y - ly; lx = x; ly = y; ldx += Math.abs(dx); ldy += Math.abs(dy);
            if (!lmoved && ldx + ldy < 10 * dp) return;   /* maybe a tap: no motion yet */
            lmoved = true;
            if (padMode) {   /* floating right stick: offset from the touch-down point, full deflection at 48 dp / sensitivity */
                float rr = 48 * dp / Math.max(0.2f, lookScale), ox = (x - lcx) / rr, oy = (y - lcy) / rr, d = (float) Math.sqrt(ox * ox + oy * oy);
                if (d > 1) { ox /= d; oy /= d; }
                GameActivity.pad.virtRight(ox, invertY ? -oy : oy);
                return;
            }
            GameActivity.nativeMouseRel(dx * lookScale, (invertY ? -dy : dy) * lookScale);
        }
    }
    private void up(int id, float x, float y, boolean cancel) {
        if (customMode && cc.up(id, x, y)) return;
        for (Btn b : btns) if (b.pointer == id) { b.pointer = -1; if (!b.toggle && !b.kbd) send(b, false); return; }
        if (id == stickPtr) { stickPtr = -1; kx = sx; ky = sy; keys(false, false, false, false); if (padMode || skyMode) GameActivity.pad.virtLeft(0, 0); return; }
        if (id == lookPtr) {
            lookPtr = -1;
            if (padMode) GameActivity.pad.virtRight(0, 0);
            if (tapClick && !cancel && !lmoved && SystemClock.uptimeMillis() - lt < 350) {   /* tap: left click at the point */
                GameActivity.nativeTouch(0, x, y); GameActivity.nativeTouch(1, x, y);
            }
        }
    }
    private void stick(float x, float y) {
        float dx = x - sx, dy = y - sy, d = (float) Math.sqrt(dx * dx + dy * dy);
        if (d > sr) { dx = dx * sr / d; dy = dy * sr / d; }
        kx = sx + dx; ky = sy + dy;
        if (padMode || skyMode) { GameActivity.pad.virtLeft(dx / sr, dy / sr); return; }
        float t = sr * 0.3f;
        /* 8 directions: a key is held when the stick is past 30% along its axis and within ~67 degrees of it */
        boolean up = -dy > t && -dy > Math.abs(dx) * 0.42f, dn = dy > t && dy > Math.abs(dx) * 0.42f;
        boolean lf = -dx > t && -dx > Math.abs(dy) * 0.42f, rt = dx > t && dx > Math.abs(dy) * 0.42f;
        keys(up, lf, dn, rt);
    }
    private void keys(boolean w, boolean a, boolean s, boolean d) {
        if (w != kw) { kw = w; GameActivity.nativeKey(KeyEvent.KEYCODE_W, w ? 1 : 0, 0); }
        if (a != ka) { ka = a; GameActivity.nativeKey(KeyEvent.KEYCODE_A, a ? 1 : 0, 0); }
        if (s != ks) { ks = s; GameActivity.nativeKey(KeyEvent.KEYCODE_S, s ? 1 : 0, 0); }
        if (d != kd) { kd = d; GameActivity.nativeKey(KeyEvent.KEYCODE_D, d ? 1 : 0, 0); }
    }
    private static void send(Btn b, boolean down) {
        if (b.kbd) return;
        if (b.isPad()) { if (b.trig != 0) GameActivity.pad.virtTrigger(b.trig, down); else GameActivity.pad.virtButton(b.padBit, down); return; }
        if (b.mouse >= 0) GameActivity.nativeMouseButton(b.mouse, down ? 1 : 0);
        else GameActivity.nativeKey(b.key, down ? 1 : 0, 0);
    }
    void releaseAll() {
        cc.releaseAll();
        for (Btn b : btns) { if (b.down()) send(b, false); b.pointer = -1; b.latched = false; }
        stickPtr = -1; kx = sx; ky = sy; keys(false, false, false, false);
        if ((padMode || skyMode) && (lookPtr >= 0 || GameActivity.pad.vBtn != 0 || GameActivity.pad.vLX != 0 || GameActivity.pad.vLY != 0)) {
            GameActivity.pad.virtLeft(0, 0); GameActivity.pad.virtRight(0, 0);
        }
        lookPtr = -1;
    }
    /** ⌨: show / hide the phone keyboard (GameActivity types into the game) */
    private void keyboard() { if (getContext() instanceof GameActivity) ((GameActivity) getContext()).toggleKeyboard(); }
    private boolean keyboardOn() { return getContext() instanceof GameActivity && ((GameActivity) getContext()).keyboardShown(); }
    void setShown(boolean s) {
        if (!s) releaseAll();
        shown = s; prefs.edit().putBoolean("shown", s).apply();
        invalidate();
    }

    /** a color with its alpha scaled by the opacity setting */
    private int a(int argb) { int al = Math.round((argb >>> 24) * alpha); return (al << 24) | (argb & 0xFFFFFF); }
    @Override protected void onDraw(Canvas c) {
        if (customMode && (shown || editing)) {   /* v1.2: elements first, ⊕ / ✓ on top */
            cc.alpha = alpha; cc.draw(c, editing);
            float sa = alpha; alpha = Math.max(alpha, 0.6f);
            drawBtn(c, toggleBtn, togglePtr >= 0 || shown || editing, editing ? "✓" : "✕");
            alpha = sa;
            if (padMode && lookPtr >= 0 && lmoved) {   /* floating right stick */
                float rr = 48 * dp / Math.max(0.2f, lookScale);
                stroke.setColor(a(0x60FFFFFF)); c.drawCircle(lcx, lcy, rr, stroke);
                fill.setColor(a(0x50FFFFFF)); c.drawCircle(lcx + Math.max(-rr, Math.min(rr, lx - lcx)), lcy + Math.max(-rr, Math.min(rr, ly - lcy)), rr * 0.35f, fill);
            }
            return;
        }
        float sa = alpha; alpha = Math.max(alpha, 0.6f);   /* ⊕ stays findable */
        drawBtn(c, toggleBtn, togglePtr >= 0 || shown || editing, editing ? "✓" : shown ? "✕" : "⊕");
        alpha = sa;
        if (!shown && !editing) { if (skyMode && !kbdBtn.hidden) drawSky(c, kbdBtn, kbdPtr >= 0 || keyboardOn()); return; }
        if (editing) { float s = alpha; alpha = 1f; banner(c); alpha = s; }
        if (skyMode) { drawSkyAll(c); return; }
        fill.setColor(a(editing && editStick ? 0x60FFB040 : 0x30FFFFFF)); c.drawCircle(sx, sy, sr, fill);
        stroke.setColor(a(0x80FFFFFF)); c.drawCircle(sx, sy, sr, stroke);
        fill.setColor(a(stickPtr >= 0 ? 0x90FFFFFF : 0x60FFFFFF)); c.drawCircle(kx, ky, sr * 0.42f, fill);
        for (Btn b : btns) if (!b.hidden) drawBtn(c, b, b.down() || (editing && b == editBtn), L.t(b.label));
        if (padMode && lookPtr >= 0 && lmoved) {   /* floating right stick */
            float rr = 48 * dp / Math.max(0.2f, lookScale);
            stroke.setColor(a(0x60FFFFFF)); c.drawCircle(lcx, lcy, rr, stroke);
            fill.setColor(a(0x50FFFFFF)); c.drawCircle(lcx + Math.max(-rr, Math.min(rr, lx - lcx)), lcy + Math.max(-rr, Math.min(rr, ly - lcy)), rr * 0.35f, fill);
        }
    }
    /* ---- "skyrim" layout: solid translucent discs, brighter while pressed ---- */
    private void drawSkyAll(Canvas c) {
        fill.setColor(a(editing && editStick ? 0x70FFB040 : 0x48FFFFFF)); c.drawCircle(sx, sy, sr, fill);
        stroke.setColor(a(0x70FFFFFF)); c.drawCircle(sx, sy, sr, stroke);
        fill.setColor(a(stickPtr >= 0 ? 0xE0FFFFFF : 0xA0FFFFFF)); c.drawCircle(kx, ky, sr * 0.45f, fill);
        for (Btn b : btns) if (!b.hidden) drawSky(c, b, b.kbd ? b.pointer >= 0 || keyboardOn() : b.down());
        if (padMode && lookPtr >= 0 && lmoved) {   /* floating right stick (camera) */
            float rr = 48 * dp / Math.max(0.2f, lookScale);
            stroke.setColor(a(0x60FFFFFF)); c.drawCircle(lcx, lcy, rr, stroke);
            fill.setColor(a(0x50FFFFFF)); c.drawCircle(lcx + Math.max(-rr, Math.min(rr, lx - lcx)), lcy + Math.max(-rr, Math.min(rr, ly - lcy)), rr * 0.35f, fill);
        }
    }
    private void drawSky(Canvas c, Btn b, boolean on) {
        int rgb = b.color & 0xFFFFFF;
        fill.setColor(a((on ? 0xF0 : 0x8C) << 24 | rgb)); c.drawCircle(b.x, b.y, b.r, fill);
        if (editing && b == editBtn) { stroke.setColor(a(0xE0FFB040)); c.drawCircle(b.x, b.y, b.r + 2 * dp, stroke); }
        int lum = (((rgb >> 16) & 255) * 299 + ((rgb >> 8) & 255) * 587 + (rgb & 255) * 114) / 1000;
        int ink = lum > 150 ? 0xE01A1D22 : 0xF0FFFFFF;
        if (b.kbd) {   /* a small keyboard: body, two rows of keys, space bar */
            float r = b.r, w = r * 1.1f, hh = r * 0.72f;
            fill.setColor(a(ink)); c.drawRoundRect(b.x - w / 2, b.y - hh / 2, b.x + w / 2, b.y + hh / 2, r * 0.12f, r * 0.12f, fill);
            fill.setColor(a(0xFF000000 | rgb)); float k = r * 0.17f, gap = r * 0.255f;
            for (int row = 0; row < 2; row++) for (int i = 0; i < 4; i++) {
                float qx = b.x + (i - 1.5f) * gap, qy = b.y - hh * 0.22f + row * gap * 0.92f;
                c.drawRect(qx - k / 2, qy - k / 2, qx + k / 2, qy + k / 2, fill);
            }
            c.drawRect(b.x - r * 0.3f, b.y + hh * 0.26f, b.x + r * 0.3f, b.y + hh * 0.26f + k * 0.8f, fill);
            return;
        }
        String label = b.label;
        float ts = label.length() > 3 ? b.r * 0.42f : label.length() > 1 ? b.r * 0.62f : b.r * 0.9f;
        text.setTextSize(ts); text.setColor(a(ink));
        c.drawText(label, b.x, b.y + ts * 0.36f, text);
    }
    private void banner(Canvas c) {
        String s = L.t("Перетащите кнопки и стик. Нажмите ✓ — готово");
        text.setTextSize(15 * dp); text.setColor(0xF0FFFFFF);
        float w = text.measureText(s) + 24 * dp, cx = getWidth() / 2f, y = getHeight() * 0.42f;
        fill.setColor(0xB0101418); c.drawRoundRect(cx - w / 2, y - 22 * dp, cx + w / 2, y + 12 * dp, 10 * dp, 10 * dp, fill);
        c.drawText(s, cx, y, text);
    }
    private void drawBtn(Canvas c, Btn b, boolean on, String label) {
        int tint = b.color != 0 ? (b.color & 0xFFFFFF) : 0xFFFFFF;
        fill.setColor(a(on ? 0x80FFB040 : (b.color != 0 ? 0x50000000 : 0x38000000) | tint)); c.drawCircle(b.x, b.y, b.r, fill);
        stroke.setColor(a(on ? 0xC0FFB040 : 0x80000000 | tint)); c.drawCircle(b.x, b.y, b.r, stroke);
        float ts = label.length() > 3 ? b.r * 0.42f : label.length() > 1 ? b.r * 0.55f : b.r * 0.8f;
        text.setTextSize(ts); text.setColor(a(0xD0FFFFFF));
        c.drawText(label, b.x, b.y + ts * 0.35f, text);
    }
}
