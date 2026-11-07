package com.me1port.launcher;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.MotionEvent;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import com.me1port.launcher.CtlProfile.El;

/** v1.2: the custom layout of ControlsOverlay (ControlsSettings layout "custom") - Winlator-style free elements from a
 * CtlProfile: buttons (4 shapes, up to 4 bindings pressed together, toggle), sticks, d-pads, trackpads and range buttons
 * (a scrollable strip of keys: 1..0, F1..F12, A..Z, numpad). Sizes follow Winlator: unit s = 1 % of the longer screen
 * side, button half sizes 3s (circle) / 4x2s (rect) / 2.5s (square), stick and trackpad 6s, d-pad 7s, range cell 4s.
 * The free area keeps the overlay's camera / tap-to-click. Editor (ControlsOverlay.editing): a toolbar (add, settings,
 * copy, delete, done), drag to move (snapped to s/2), two fingers to resize, a tap on the selected element opens its
 * settings (CtlDialogs.element); every change is saved to the profile file at once. */
final class CustomControls {
    interface Host { void tap(); void keyboard(); boolean keyboardOn(); void editElement(El e); void editDone(); void invalidate(); android.content.Context ctx(); }
    private final Host host; private final float dp;
    CtlProfile prof; ArrayList<El> els = new ArrayList<>();
    private int W, H; private float s = 10;
    float size = 1, stickSize = 1, look = 1.2f, alpha = 1; boolean mirror, invertY, tapClick = true;
    /* editor */
    El sel; private int ePtr = -1, ePtr2 = -1; private float edx, edy, ex0, ey0, pinch0, scale0; private boolean eMoved, wasSel;
    private static final String[] TB = { "Добавить", "Настроить", "Копия", "Удалить", "Готово" };
    private final RectF[] tb = new RectF[TB.length];
    private final HashMap<String, Integer> held = new HashMap<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path(); private final RectF rf = new RectF();
    private final Handler h = new Handler(Looper.getMainLooper()); private boolean ticking, usedPad;

    CustomControls(Host host, float dp) {
        this.host = host; this.dp = dp;
        stroke.setStyle(Paint.Style.STROKE); text.setTextAlign(Paint.Align.CENTER); text.setFakeBoldText(true);
        for (int i = 0; i < tb.length; i++) tb[i] = new RectF();
    }
    void load() {
        releaseAll();
        prof = CtlProfile.active(host.ctx()); els = prof.els; sel = null;
        layout(W, H);
    }
    void save() { if (prof != null) CtlProfile.save(host.ctx(), prof); }

    /* ---- geometry ---- */
    void layout(int w, int h) {
        W = w; H = h; if (w <= 0 || h <= 0) return;
        s = Math.max(w, h) / 100f;
        for (El e : els) place(e);
    }
    void place(El e) {
        float k = s * e.scale * (e.is("STICK") ? stickSize : size), hw, hh;
        switch (e.type) {
        case "BUTTON":
            if (e.shape.equals("RECT") || e.shape.equals("ROUND_RECT")) { hw = 4 * k; hh = 2 * k; }
            else if (e.shape.equals("SQUARE")) hw = hh = 2.5f * k;
            else hw = hh = 3 * k;
            break;
        case "D_PAD": hw = hh = 7 * k; break;
        case "RANGE_BUTTON": hw = 2 * k * e.cells; hh = 2 * k; if (e.orient == 1) { float t = hw; hw = hh; hh = t; } break;
        default: hw = hh = 6 * k;   /* STICK, TRACKPAD */
        }
        e.hw = hw; e.hh = hh; e.cx = (mirror ? 1 - e.x : e.x) * W; e.cy = e.y * H;
    }
    private static boolean round(El e) { return e.is("STICK") || (e.is("BUTTON") && e.shape.equals("CIRCLE")); }
    private static boolean hit(El e, float x, float y, float slack) {
        float dx = x - e.cx, dy = y - e.cy;
        if (round(e)) { float r = e.hw + slack; return dx * dx + dy * dy <= r * r; }
        return Math.abs(dx) <= e.hw + slack && Math.abs(dy) <= e.hh + slack;
    }
    private El at(float x, float y, boolean free) {
        for (int i = els.size() - 1; i >= 0; i--) { El e = els.get(i); if (!e.hidden && (!free || e.ptr < 0) && hit(e, x, y, 4 * dp)) return e; }
        return null;
    }
    boolean contains(float x, float y) { return at(x, y, false) != null; }
    boolean keyboardDown(int id, float x, float y) {
        for (El e : els) if (!e.hidden && e.ptr < 0 && first(e).kind == Bind.KBD && hit(e, x, y, 4 * dp)) {
            e.ptr = id; host.tap(); button(e, true); return true;
        }
        return false;
    }
    void drawKeyboard(Canvas c) { for (El e : els) if (!e.hidden && first(e).kind == Bind.KBD) drawEl(c, e, false); }

    /* ---- input ---- */
    boolean down(int id, float x, float y) {
        El e = at(x, y, true);
        if (e == null) for (El q : els) if (!q.hidden && q.zone && q.is("STICK") && q.ptr < 0 && (x < W / 2f) == (q.cx < W / 2f)) { e = q; break; }
        if (e == null) return false;
        e.ptr = id; e.moved = false; e.t0 = SystemClock.uptimeMillis(); e.lx = e.dx0 = x; e.ly = e.dy0 = y;
        host.tap();
        switch (e.type) {
        case "BUTTON": if (e.toggle) { e.latched = !e.latched; button(e, e.latched); } else button(e, true); break;
        case "STICK": stick(e, x, y); break;
        case "D_PAD": dpad(e, x, y); break;
        case "RANGE_BUTTON": {
            e.scroll0 = e.scroll; e.cell = cellAt(e, x, y); e.cellDown = false;
            final El fe = e;   /* held without moving: the key goes down (and stays down until the finger lifts) */
            e.task = () -> { if (fe.ptr >= 0 && !fe.moved && fe.cell >= 0 && !fe.cellDown) { fe.cellDown = true; press(cellBind(fe), true); host.invalidate(); } };
            h.postDelayed(e.task, 120);
            break;
        }
        default: break;
        }
        return true;
    }
    boolean move(int id, float x, float y) {
        El e = null; for (El q : els) if (q.ptr == id) { e = q; break; }
        if (e == null) return false;
        if (prof != null && prof.slide && e.is("BUTTON") && !e.toggle && first(e).kind != Bind.KBD) {
            El next = at(x, y, true);
            if (next != null && next.is("BUTTON") && !next.toggle && first(next).kind != Bind.KBD && hit(next, x, y, 0)) {
                button(e, false); e.ptr = -1; next.ptr = id; next.dx0 = x; next.dy0 = y; button(next, true); host.tap(); return true;
            }
        }
        if (!e.moved && Math.abs(x - e.dx0) + Math.abs(y - e.dy0) > 10 * dp) e.moved = true;
        switch (e.type) {
        case "STICK": stick(e, x, y); break;
        case "D_PAD": dpad(e, x, y); break;
        case "TRACKPAD": trackpad(e, x, y); break;
        case "RANGE_BUTTON": {
            if (!e.moved) break;
            if (e.task != null) { h.removeCallbacks(e.task); e.task = null; }
            if (e.cellDown) { e.cellDown = false; press(cellBind(e), false); }
            float d = e.orient == 0 ? x - e.dx0 : y - e.dy0, cs = cellSize(e);
            e.scroll = CtlProfile.clamp(e.scroll0 - d / cs, 0, Math.max(0, e.rangeKeys().length - e.cells));
            break;
        }
        default: break;
        }
        return true;
    }
    boolean up(int id, float x, float y) {
        El e = null; for (El q : els) if (q.ptr == id) { e = q; break; }
        if (e == null) return false;
        e.ptr = -1;
        switch (e.type) {
        case "BUTTON": if (!e.toggle) button(e, false); break;
        case "STICK": case "D_PAD": e.nx = e.ny = 0; dirs(e, 0, 0, 1); break;
        case "TRACKPAD":
            dirs(e, 0, 0, 1);
            if (!e.moved && tapClick && SystemClock.uptimeMillis() - e.t0 < 300 && first(e).kind <= Bind.MOVE) {   /* tap = left click */
                GameActivity.nativeMouseButton(0, 1); h.postDelayed(() -> GameActivity.nativeMouseButton(0, 0), 60);
            }
            break;
        case "RANGE_BUTTON": {
            if (e.task != null) { h.removeCallbacks(e.task); e.task = null; }
            final Bind b = cellBind(e);
            if (e.cellDown) { e.cellDown = false; press(b, false); }
            else if (!e.moved && e.cell >= 0) { press(b, true); h.postDelayed(() -> press(b, false), 80); }   /* a quick tap */
            e.cell = -1;
            break;
        }
        default: break;
        }
        return true;
    }
    void releaseAll() {
        for (El e : els) {
            if (e.task != null) { h.removeCallbacks(e.task); e.task = null; }
            e.ptr = -1; e.latched = false; e.pressed = false; e.cellDown = false; e.cell = -1; e.nx = e.ny = 0; e.mvx = e.mvy = 0; e.dirs = 0;
        }
        for (Map.Entry<String, Integer> m : held.entrySet()) if (m.getValue() > 0) send(Bind.of(m.getKey()), false);
        held.clear();
        if (usedPad) { usedPad = false; GameActivity.pad.virtLeft(0, 0); GameActivity.pad.virtRight(0, 0); }
    }

    private static Bind first(El e) { for (Bind b : e.res) if (b != null && !b.none()) return b; return Bind.NO; }
    private void button(El e, boolean on) {
        e.pressed = on; float mx = 0, my = 0;
        if (on) { for (Bind b : e.res) { press(b, true); if (b.kind == Bind.MOVE) { mx += DX[b.code]; my += DY[b.code]; } } }
        else for (int i = 3; i >= 0; i--) press(e.res[i], false);
        e.mvx = mx; e.mvy = my; if (mx != 0 || my != 0) startTick();
    }
    private static final float[] DX = { 0, 1, 0, -1 }, DY = { -1, 0, 1, 0 };
    /** directional elements: nx, ny in -1..1; slots up / right / down / left */
    private void dirs(El e, float nx, float ny, float thr) {
        Bind b0 = first(e);
        if (b0.kind == Bind.LSTICK) { usedPad = true; GameActivity.pad.virtLeft(nx, ny); return; }
        if (b0.kind == Bind.RSTICK) { usedPad = true; GameActivity.pad.virtRight(nx, invertY ? -ny : ny); return; }
        if (b0.kind == Bind.MOVE) { e.mvx = nx; e.mvy = ny; if (nx != 0 || ny != 0) startTick(); return; }
        float ax = Math.abs(nx), ay = Math.abs(ny);
        int d = (-ny > thr && ay > ax * 0.42f ? 1 : 0) | (nx > thr && ax > ay * 0.42f ? 2 : 0) | (ny > thr && ay > ax * 0.42f ? 4 : 0) | (-nx > thr && ax > ay * 0.42f ? 8 : 0);
        int ch = d ^ e.dirs; e.dirs = d;
        for (int i = 0; i < 4; i++) if ((ch & (1 << i)) != 0) press(e.res[i], (d & (1 << i)) != 0);
    }
    private void stick(El e, float x, float y) {
        float dx = x - e.cx, dy = y - e.cy, r = e.hw, d = (float) Math.sqrt(dx * dx + dy * dy);
        if (d > r) { dx = dx * r / d; dy = dy * r / d; }
        e.nx = dx / r; e.ny = dy / r; dirs(e, e.nx, e.ny, 0.3f);
    }
    private void dpad(El e, float x, float y) {
        e.nx = CtlProfile.clamp((x - e.cx) / e.hw, -1, 1); e.ny = CtlProfile.clamp((y - e.cy) / e.hh, -1, 1);
        dirs(e, e.nx, e.ny, 0.22f);
    }
    private void trackpad(El e, float x, float y) {
        float dx = x - e.lx, dy = y - e.ly; e.lx = x; e.ly = y;
        Bind b0 = first(e);
        if (b0.kind == Bind.NONE || b0.kind == Bind.MOVE) {   /* the mouse (camera), like the free area */
            float k = look * (prof != null ? prof.cursorSpeed : 1);
            GameActivity.nativeMouseRel(dx * k, (invertY ? -dy : dy) * k);
            return;
        }
        float r = Math.max(24 * dp, e.hw * 0.5f);   /* other bindings: a floating stick around the touch-down point */
        float nx = (x - e.dx0) / r, ny = (y - e.dy0) / r, d = (float) Math.sqrt(nx * nx + ny * ny);
        if (d > 1) { nx /= d; ny /= d; }
        e.nx = nx; e.ny = ny; dirs(e, nx, ny, 0.3f);
    }
    private float cellSize(El e) { return (e.orient == 0 ? 2 * e.hw : 2 * e.hh) / e.cells; }
    private int cellAt(El e, float x, float y) {
        float p = e.orient == 0 ? x - (e.cx - e.hw) : y - (e.cy - e.hh);
        int i = (int) Math.floor(p / cellSize(e) + e.scroll), n = e.rangeKeys().length;
        return i < 0 || i >= n ? -1 : i;
    }
    private static Bind cellBind(El e) { String[] k = e.rangeKeys(); return e.cell >= 0 && e.cell < k.length ? Bind.of(k[e.cell]) : Bind.NO; }

    /** press / release with a count per binding: two elements holding the same key release it only when both let go */
    private void press(Bind b, boolean down) {
        if (b == null || b.none() || b.kind == Bind.MOVE) return;
        if (b.kind == Bind.KBD) { if (down) host.keyboard(); return; }
        if (b.kind == Bind.WHEEL) { if (down) GameActivity.nativeMouseButton(b.code > 0 ? 3 : 4, 1); return; }
        Integer n = held.get(b.name); int c = n == null ? 0 : n;
        if (down) { held.put(b.name, c + 1); if (c > 0) return; }
        else { if (c <= 0) return; held.put(b.name, c - 1); if (c > 1) return; }
        send(b, down);
    }
    private void send(Bind b, boolean d) {
        if (b.kind >= Bind.PAD && b.kind <= Bind.RSTICK) usedPad = true;
        switch (b.kind) {
        case Bind.KEY: GameActivity.nativeKey(b.code, d ? 1 : 0, 0); break;
        case Bind.MOUSE: GameActivity.nativeMouseButton(b.code, d ? 1 : 0); break;
        case Bind.PAD: GameActivity.pad.virtButton(b.code, d); break;
        case Bind.TRIG: GameActivity.pad.virtTrigger(b.code, d); break;
        case Bind.LSTICK: GameActivity.pad.virtLeft(d ? DX[b.code] : 0, d ? DY[b.code] : 0); break;
        case Bind.RSTICK: GameActivity.pad.virtRight(d ? DX[b.code] : 0, d ? (invertY ? -DY[b.code] : DY[b.code]) : 0); break;
        default: break;
        }
    }
    /** MOUSE_MOVE_* bindings: relative mouse motion 60 times a second while held (quadratic, like Pad's right stick) */
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            float sx = 0, sy = 0; for (El e : els) { sx += e.mvx; sy += e.mvy; }
            if (sx == 0 && sy == 0) { ticking = false; return; }
            float k = 16f * look * (prof != null ? prof.cursorSpeed : 1);
            GameActivity.nativeMouseRel(Math.signum(sx) * sx * sx * k, (invertY ? -1 : 1) * Math.signum(sy) * sy * sy * k);
            h.postDelayed(this, 16);
        }
    };
    private void startTick() { if (!ticking) { ticking = true; h.post(tick); } }

    /* ---- drawing ---- */
    private int a(int argb) { int al = Math.round((argb >>> 24) * alpha); return (al << 24) | (argb & 0xFFFFFF); }
    private void fitText(String t, float maxW, float maxH) {
        text.setTextSize(maxH);
        float w = text.measureText(t);
        if (w > maxW && w > 0) text.setTextSize(maxH * maxW / w);
    }
    private void label(Canvas c, String t, float x, float y, float maxW, float maxH, int color) {
        if (t == null || t.isEmpty()) return;
        fitText(t, maxW, maxH); text.setColor(a(color));
        c.drawText(t, x, y + text.getTextSize() * 0.36f, text);
    }
    String labelOf(El e) {
        if (e.text != null && !e.text.isEmpty()) return e.text;
        StringBuilder b = new StringBuilder();
        for (Bind q : e.res) if (q != null && !q.none()) { if (b.length() > 0) b.append('+'); b.append(q.label()); }
        return b.toString();
    }
    void draw(Canvas c, boolean editing) {
        if (editing) {   /* the snapping grid */
            stroke.setStrokeWidth(1); stroke.setColor(0x14FFFFFF);
            for (float x = 0; x < W; x += 4 * s) c.drawLine(x, 0, x, H, stroke);
            for (float y = 0; y < H; y += 4 * s) c.drawLine(0, y, W, y, stroke);
        }
        for (El e : els) if (!e.hidden) drawEl(c, e, editing);
        if (editing) drawEditor(c);
    }
    private static final int FILL = 0x38101418, FILL_ON = 0x80FFB040, LINE = 0xB0FFFFFF, LINE_ON = 0xE0FFB040, INK = 0xE8FFFFFF, SEL = 0xFF40C4FF;
    private void drawEl(Canvas c, El e, boolean editing) {
        boolean on = !editing && (e.pressed || e.latched || (e.ptr >= 0 && !e.is("RANGE_BUTTON")));
        float lw = 2 * dp; stroke.setStrokeWidth(lw);
        int line = editing && e == sel ? SEL : on ? LINE_ON : LINE;
        if (editing && e == sel) stroke.setStrokeWidth(3 * dp);
        stroke.setColor(editing && e == sel ? SEL : a(line));
        float l = e.cx - e.hw, t = e.cy - e.hh, r = e.cx + e.hw, b = e.cy + e.hh;
        switch (e.type) {
        case "BUTTON": {
            fill.setColor(a(on ? FILL_ON : e.color != 0 ? e.color : FILL));
            if (e.shape.equals("CIRCLE")) { c.drawCircle(e.cx, e.cy, e.hw, fill); c.drawCircle(e.cx, e.cy, e.hw, stroke); }
            else {
                float rr = e.shape.equals("ROUND_RECT") ? e.hh : e.shape.equals("SQUARE") ? e.hh * 0.18f : 0;
                rf.set(l, t, r, b); c.drawRoundRect(rf, rr, rr, fill); c.drawRoundRect(rf, rr, rr, stroke);
            }
            label(c, labelOf(e), e.cx, e.cy, e.hw * 1.6f, e.hh * (e.shape.equals("CIRCLE") ? 0.8f : 0.9f), e.color != 0 && !on && (((e.color >> 16) & 255) * 299 + ((e.color >> 8) & 255) * 587 + (e.color & 255) * 114) > 145000 ? 0xFF15191E : INK);
            if (e.toggle) { fill.setColor(a(e.latched ? 0xF0FFB040 : 0x80FFFFFF)); c.drawCircle(e.cx, e.cy + e.hh * 0.72f, Math.max(2 * dp, e.hh * 0.07f), fill); }
            break;
        }
        case "STICK": {
            fill.setColor(a(FILL)); c.drawCircle(e.cx, e.cy, e.hw, fill); c.drawCircle(e.cx, e.cy, e.hw, stroke);
            fill.setColor(a(e.ptr >= 0 && !editing ? 0xC0FFFFFF : 0x70FFFFFF));
            c.drawCircle(e.cx + e.nx * e.hw, e.cy + e.ny * e.hw, e.hw * 0.42f, fill);
            if (editing) label(c, labelOf(e), e.cx, e.cy + e.hh * 0.72f, e.hw * 1.4f, e.hh * 0.16f, INK);
            break;
        }
        case "D_PAD": {
            float wa = e.hw / 3f;   /* arm half width */
            for (int i = 0; i < 4; i++) {
                boolean di = !editing && (e.dirs & (1 << i)) != 0;
                switch (i) {
                case 0: rf.set(e.cx - wa, t, e.cx + wa, e.cy - wa); break;
                case 1: rf.set(e.cx + wa, e.cy - wa, r, e.cy + wa); break;
                case 2: rf.set(e.cx - wa, e.cy + wa, e.cx + wa, b); break;
                default: rf.set(l, e.cy - wa, e.cx - wa, e.cy + wa); break;
                }
                fill.setColor(a(di ? FILL_ON : FILL)); c.drawRoundRect(rf, wa * 0.3f, wa * 0.3f, fill);
                stroke.setColor(editing && e == sel ? SEL : a(di ? LINE_ON : LINE)); c.drawRoundRect(rf, wa * 0.3f, wa * 0.3f, stroke);
                Bind q = e.res[i]; String lb = q == null || q.none() ? "" : q.label();
                label(c, lb, rf.centerX(), rf.centerY(), rf.width() * 0.85f, Math.min(rf.width(), rf.height()) * 0.42f, INK);
            }
            fill.setColor(a(FILL)); c.drawRect(e.cx - wa, e.cy - wa, e.cx + wa, e.cy + wa, fill);
            break;
        }
        case "TRACKPAD": {
            fill.setColor(a(e.ptr >= 0 && !editing ? 0x50FFFFFF : FILL)); rf.set(l, t, r, b);
            c.drawRoundRect(rf, e.hw * 0.12f, e.hw * 0.12f, fill); c.drawRoundRect(rf, e.hw * 0.12f, e.hw * 0.12f, stroke);
            String lb = e.text != null && !e.text.isEmpty() ? e.text : first(e).none() || first(e).kind == Bind.MOVE ? L.t("Тачпад") : labelOf(e);
            label(c, lb, e.cx, e.cy, e.hw * 1.5f, e.hh * 0.2f, 0x90FFFFFF);
            break;
        }
        case "RANGE_BUTTON": {
            fill.setColor(a(FILL)); rf.set(l, t, r, b);
            c.drawRoundRect(rf, Math.min(e.hw, e.hh) * 0.25f, Math.min(e.hw, e.hh) * 0.25f, fill);
            String[] keys = e.rangeKeys(); float cs = cellSize(e);
            c.save(); c.clipRect(l, t, r, b);
            int i0 = (int) Math.floor(e.scroll);
            for (int i = Math.max(0, i0); i < keys.length && i <= i0 + e.cells; i++) {
                float p = (i - e.scroll) * cs, x0 = e.orient == 0 ? l + p : l, y0 = e.orient == 0 ? t : t + p;
                float x1 = e.orient == 0 ? x0 + cs : r, y1 = e.orient == 0 ? b : y0 + cs;
                boolean di = !editing && e.cellDown && e.cell == i;
                if (di) { fill.setColor(a(FILL_ON)); c.drawRect(x0, y0, x1, y1, fill); }
                stroke.setStrokeWidth(1 * dp); stroke.setColor(a(0x60FFFFFF));
                if (e.orient == 0 && i > i0) c.drawLine(x0, t + cs * 0.15f, x0, b - cs * 0.15f, stroke);
                if (e.orient == 1 && i > i0) c.drawLine(l + cs * 0.15f, y0, r - cs * 0.15f, y0, stroke);
                label(c, Bind.of(keys[i]).label(), (x0 + x1) / 2, (y0 + y1) / 2, cs * 0.8f, cs * 0.42f, INK);
            }
            c.restore();
            stroke.setStrokeWidth(editing && e == sel ? 3 * dp : lw); stroke.setColor(editing && e == sel ? SEL : a(LINE));
            c.drawRoundRect(rf, Math.min(e.hw, e.hh) * 0.25f, Math.min(e.hw, e.hh) * 0.25f, stroke);
            /* more keys beyond the edges: small arrows */
            fill.setColor(a(0xB0FFFFFF)); float ar = 3 * dp;
            if (e.scroll > 0.01f) { if (e.orient == 0) tri(c, l + ar * 1.5f, e.cy, -1, 0, ar); else tri(c, e.cx, t + ar * 1.5f, 0, -1, ar); }
            if (e.scroll < keys.length - e.cells - 0.01f) { if (e.orient == 0) tri(c, r - ar * 1.5f, e.cy, 1, 0, ar); else tri(c, e.cx, b - ar * 1.5f, 0, 1, ar); }
            break;
        }
        default: break;
        }
    }
    private void tri(Canvas c, float x, float y, int dx, int dy, float r) {
        path.reset();
        if (dx != 0) { path.moveTo(x + dx * r, y); path.lineTo(x - dx * r, y - r); path.lineTo(x - dx * r, y + r); }
        else { path.moveTo(x, y + dy * r); path.lineTo(x - r, y - dy * r); path.lineTo(x + r, y - dy * r); }
        path.close(); c.drawPath(path, fill);
    }
    private void drawEditor(Canvas c) {
        text.setTextSize(14 * dp);
        float total = 0, gap = 8 * dp, hgt = 38 * dp;
        float[] wd = new float[TB.length];
        for (int i = 0; i < TB.length; i++) { wd[i] = text.measureText(L.t(TB[i])) + 26 * dp; total += wd[i] + (i > 0 ? gap : 0); }
        float x = (W - total) / 2f, y = 10 * dp;
        for (int i = 0; i < TB.length; i++) {
            tb[i].set(x, y, x + wd[i], y + hgt); x += wd[i] + gap;
            boolean en = i == 0 || i == 4 || sel != null;
            fill.setColor(i == 4 ? 0xE02E7D32 : 0xE0202830); c.drawRoundRect(tb[i], 9 * dp, 9 * dp, fill);
            stroke.setStrokeWidth(1 * dp); stroke.setColor(0x80FFFFFF); c.drawRoundRect(tb[i], 9 * dp, 9 * dp, stroke);
            text.setTextSize(14 * dp); text.setColor(en ? 0xFFFFFFFF : 0x60FFFFFF);
            c.drawText(L.t(TB[i]), tb[i].centerX(), tb[i].centerY() + 5 * dp, text);
        }
        String hint = sel == null ? L.t("Коснитесь элемента, чтобы выбрать его; «Добавить» — новый элемент")
                : L.t("Перетаскивайте; двумя пальцами — размер; ещё одно касание — настройки") + "   [" + labelOf(sel) + "]";
        text.setTextSize(12 * dp); float tw = text.measureText(hint) + 20 * dp, cy = y + hgt + 20 * dp;
        fill.setColor(0xB0101418); c.drawRoundRect(W / 2f - tw / 2, cy - 15 * dp, W / 2f + tw / 2, cy + 7 * dp, 8 * dp, 8 * dp, fill);
        text.setColor(0xF0FFFFFF); c.drawText(hint, W / 2f, cy, text);
    }

    /* ---- editor input ---- */
    void edit(int act, int id, float x, float y, MotionEvent ev) {
        if (act == MotionEvent.ACTION_DOWN || act == MotionEvent.ACTION_POINTER_DOWN) {
            if (ePtr >= 0) {   /* second finger: resize the selected element */
                if (sel != null && ePtr2 < 0) { ePtr2 = id; pinch0 = dist(ev); scale0 = sel.scale; eMoved = true; }
                return;
            }
            for (int i = 0; i < tb.length; i++) if (tb[i].contains(x, y)) { host.tap(); toolbar(i); return; }
            El e = at(x, y, false);
            if (e == null) { sel = null; return; }
            wasSel = e == sel; sel = e; ePtr = id; edx = e.cx - x; edy = e.cy - y; ex0 = x; ey0 = y; eMoved = false; host.tap();
        } else if (act == MotionEvent.ACTION_MOVE) {
            if (sel == null) return;
            if (ePtr2 >= 0) {
                float d = dist(ev);
                if (pinch0 > 0 && d > 0) { sel.scale = CtlProfile.clamp(Math.round(scale0 * d / pinch0 * 20) / 20f, 0.3f, 4f); place(sel); }
                return;
            }
            if (ePtr < 0) return;
            int i = ev.findPointerIndex(ePtr); if (i < 0) return;
            float px = ev.getX(i), py = ev.getY(i);
            if (!eMoved && Math.abs(px - ex0) + Math.abs(py - ey0) < 8 * dp) return;
            eMoved = true;
            float g = s / 2f, nx = Math.round((px + edx) / g) * g, ny = Math.round((py + edy) / g) * g;
            nx = Math.max(0, Math.min(W, nx)); ny = Math.max(0, Math.min(H, ny));
            sel.x = (mirror ? W - nx : nx) / Math.max(1, W); sel.y = ny / Math.max(1, H); place(sel);
        } else if (act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_POINTER_UP || act == MotionEvent.ACTION_CANCEL) {
            if (id == ePtr2) { ePtr2 = -1; save(); return; }
            if (id != ePtr) return;
            ePtr = -1; ePtr2 = -1;
            if (eMoved) save();
            else if (wasSel && sel != null && act != MotionEvent.ACTION_CANCEL) host.editElement(sel);
        }
    }
    private float dist(MotionEvent ev) {
        int a = ev.findPointerIndex(ePtr), b = ev.findPointerIndex(ePtr2);
        if (a < 0 || b < 0) return 0;
        float dx = ev.getX(a) - ev.getX(b), dy = ev.getY(a) - ev.getY(b);
        return (float) Math.sqrt(dx * dx + dy * dy);
    }
    private void toolbar(int i) {
        switch (i) {
        case 0: {
            El e = new El(); e.x = 0.5f; e.y = 0.55f; e.resolve();
            els.add(e); place(e); sel = e; save(); host.editElement(e);
            break;
        }
        case 1: if (sel != null) host.editElement(sel); break;
        case 2: if (sel != null) {
            El e = sel.copy(); e.x = Math.min(0.97f, sel.x + 0.06f); e.y = Math.min(0.97f, sel.y + 0.06f);
            els.add(e); place(e); sel = e; save();
        } break;
        case 3: if (sel != null) { els.remove(sel); sel = null; save(); } break;
        default: host.editDone(); break;
        }
    }
    void remove(El e) { els.remove(e); if (sel == e) sel = null; save(); }
}
