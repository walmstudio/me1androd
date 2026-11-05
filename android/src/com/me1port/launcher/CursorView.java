package com.me1port.launcher;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.View;

/** v1.1.5: the game's mouse cursor. ME1 has no cursor of its own: on Windows the menus (main menu, inventory,
 * squad, map, dialogue) show the system cursor, and the game hides it with ShowCursor(FALSE) while it captures
 * the mouse (camera). The runtime emulates the ShowCursor count (hle_ui_cursor); this view polls it every frame
 * and draws a Windows-like arrow at the cursor position. Touches go through (not clickable).
 * Mode: ControlsSettings "cursor" - "auto" (when the game shows it, default), "always", "off". */
final class CursorView extends View {
    private final SharedPreferences prefs;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path arrow = new Path();
    private final float scale;
    private boolean running, shown;
    private float cx, cy;
    private final Runnable tick = new Runnable() { public void run() { poll(); if (running) postOnAnimation(this); } };

    CursorView(Context c, SharedPreferences prefs) {
        super(c);
        this.prefs = prefs;
        setClickable(false); setFocusable(false); setWillNotDraw(false);
        scale = 1.3f * c.getResources().getDisplayMetrics().density;   /* one arrow pixel; the arrow is 19 px (~25 dp) tall */
        fill.setColor(Color.WHITE); fill.setStyle(Paint.Style.FILL);
        edge.setColor(Color.BLACK); edge.setStyle(Paint.Style.STROKE); edge.setStrokeJoin(Paint.Join.MITER);
        edge.setStrokeWidth(Math.max(1f, scale));
        /* the standard Windows arrow, tip at (0, 0) */
        final float[] pts = { 0, 0, 0, 16, 4, 12.5f, 7, 19, 9.5f, 18, 6.6f, 11.5f, 11.5f, 11.5f };
        arrow.moveTo(pts[0] * scale, pts[1] * scale);
        for (int i = 2; i < pts.length; i += 2) arrow.lineTo(pts[i] * scale, pts[i + 1] * scale);
        arrow.close();
    }

    void start() { if (!running) { running = true; postOnAnimation(tick); } }
    void stop() { running = false; removeCallbacks(tick); if (shown) { shown = false; invalidate(); } }

    private void poll() {
        String mode = prefs.getString("cursor", "auto");
        long v;
        try { v = GameActivity.nativeCursor(); } catch (Throwable t) { v = -1; }
        int st = v < 0 ? -1 : (int) (v >>> 42) - 1;
        boolean show = st >= 0 && !"off".equals(mode) && (st == 1 || "always".equals(mode));
        float x = (float) ((v >>> 21) & 0x1FFFFF), y = (float) (v & 0x1FFFFF);
        if (show != shown || (show && (x != cx || y != cy))) { shown = show; cx = x; cy = y; invalidate(); }
    }

    @Override protected void onDraw(Canvas c) {
        if (!shown) return;
        c.save(); c.translate(cx, cy);
        c.drawPath(arrow, fill); c.drawPath(arrow, edge);
        c.restore();
    }
}
