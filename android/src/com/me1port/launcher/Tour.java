package com.me1port.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;

/** A read-only tour; it never changes game settings. */
final class Tour {
    private final Activity a; ScrollView scroll;
    private static final class Step { View anchor; String title, body; Step(View v, String t, String b) { anchor=v; title=t; body=b; } }
    private final ArrayList<Step> steps = new ArrayList<>();
    private int current; private AlertDialog dialog; private View highlighted; private Drawable oldBackground;
    Tour(Activity activity) { a = activity; }
    void add(View view, String title, String body) { steps.add(new Step(view,title,body)); }
    void show() { if (steps.isEmpty() || a.isFinishing()) return; current = 0; showStep(); }
    private void clearHighlight() { if (highlighted != null) { highlighted.setBackground(oldBackground); highlighted = null; oldBackground = null; } }
    private void showStep() {
        clearHighlight(); Step s = steps.get(current); highlighted=s.anchor; oldBackground=s.anchor.getBackground();
        s.anchor.setBackgroundColor(0x554FA3E0);
        if (scroll != null) scroll.post(() -> { android.graphics.Rect r = new android.graphics.Rect(); s.anchor.getDrawingRect(r); ((android.view.ViewGroup)scroll.getChildAt(0)).offsetDescendantRectToMyCoords(s.anchor,r); scroll.smoothScrollTo(0, Math.max(0, r.top - 20)); });
        TextView message = new TextView(a); int pad=(int)(20*a.getResources().getDisplayMetrics().density); message.setPadding(pad,pad/2,pad,pad/2); message.setText(s.body); message.setTextColor(Color.WHITE); message.setTextSize(15);
        ScrollView contents = new ScrollView(a); contents.addView(message);
        dialog = new AlertDialog.Builder(a).setTitle((current+1)+" / "+steps.size()+" · "+s.title).setView(contents)
                .setNegativeButton(L.t("Назад"), null).setPositiveButton(L.t(current == steps.size()-1 ? "Готово" : "Далее"), null).setNeutralButton(L.t("Закрыть"), null).create();
        dialog.setOnDismissListener(d -> clearHighlight());
        dialog.show(); Window w=dialog.getWindow(); if (w != null) { w.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND); w.setGravity(Gravity.BOTTOM); w.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.WRAP_CONTENT); }
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(current>0);
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> { dialog.dismiss(); current--; showStep(); });
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> { dialog.dismiss(); if (++current < steps.size()) showStep(); });
    }
}
