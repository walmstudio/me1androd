package com.me1port.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.Locale;
import com.me1port.launcher.CtlProfile.El;

/** v1.2: dialogs of the custom layout: element settings (type, shape, bindings, size, toggle, label, range) with the
 * binding picker, and the profile manager (choose, new, copy, rename, delete, import / export Winlator .icp). */
final class CtlDialogs {
    private CtlDialogs() {}
    interface Pick { void on(String name); }
    private static final String[] TYPE_L = { "Кнопка", "Крестовина", "Стик", "Тачпад", "Ряд клавиш" };
    private static final String[] SHAPE_L = { "Круг", "Прямоугольник", "Скруглённый прямоугольник", "Квадрат" };
    private static final String[] RANGE_L = { "1 … 0", "F1 … F12", "A … Z", "Цифровой блок 0 … 9" };
    private static final String[] DIR_L = { "Вверх", "Вправо", "Вниз", "Влево" };
    private static final String[][] DIR_PRESETS = {
        { "KEY_W", "KEY_D", "KEY_S", "KEY_A" }, { "KEY_UP", "KEY_RIGHT", "KEY_DOWN", "KEY_LEFT" },
        { "MOUSE_MOVE_UP", "MOUSE_MOVE_RIGHT", "MOUSE_MOVE_DOWN", "MOUSE_MOVE_LEFT" },
        { "GAMEPAD_LEFT_THUMB_UP", "GAMEPAD_LEFT_THUMB_RIGHT", "GAMEPAD_LEFT_THUMB_DOWN", "GAMEPAD_LEFT_THUMB_LEFT" },
        { "GAMEPAD_RIGHT_THUMB_UP", "GAMEPAD_RIGHT_THUMB_RIGHT", "GAMEPAD_RIGHT_THUMB_DOWN", "GAMEPAD_RIGHT_THUMB_LEFT" },
        { "GAMEPAD_DPAD_UP", "GAMEPAD_DPAD_RIGHT", "GAMEPAD_DPAD_DOWN", "GAMEPAD_DPAD_LEFT" } };
    private static final String[] DIR_PRESET_L = { "WASD", "Стрелки", "Мышь", "Левый стик", "Правый стик", "Крестовина геймпада" };

    private static int px(Activity a, float v) { return (int) (v * a.getResources().getDisplayMetrics().density); }
    private static TextView head(Activity a, String t) {
        TextView v = new TextView(a); v.setText(t); v.setTextSize(15); v.setPadding(0, px(a, 10), 0, px(a, 2)); return v;
    }
    private static Spinner spinner(Activity a, String[] labels, int sel) {
        Spinner sp = new Spinner(a);
        ArrayAdapter<String> ad = new ArrayAdapter<>(a, android.R.layout.simple_spinner_item, L.arr(labels));
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad); sp.setSelection(Math.max(0, sel));
        return sp;
    }
    private static int idx(String[] all, String v) { for (int i = 0; i < all.length; i++) if (all[i].equals(v)) return i; return 0; }
    interface Sel { void on(int i); }
    private static void onSel(Spinner sp, final Sel s) {
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int i, long id) { s.on(i); }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });
    }
    private static Button btn(Activity a, String t, View.OnClickListener l) {
        Button b = new Button(a); b.setText(t); b.setAllCaps(false); b.setOnClickListener(l); return b;
    }

    /** the binding picker: a category, then the binding */
    static void pickBinding(final Activity a, final Pick done) {
        String[] cats = new String[Bind.CATS.length + 1];
        cats[0] = L.t("Нет (ничего)");
        for (int i = 0; i < Bind.CATS.length; i++) cats[i + 1] = L.t(Bind.CATS[i]);
        new AlertDialog.Builder(a).setTitle(L.t("Что нажимать")).setItems(cats, (d, w) -> {
            if (w == 0) { done.on("NONE"); return; }
            final ArrayList<Bind> l = Bind.inCat(w - 1);
            String[] n = new String[l.size()];
            for (int i = 0; i < n.length; i++) n[i] = l.get(i).desc();
            new AlertDialog.Builder(a).setTitle(cats[w]).setItems(n, (d2, k) -> done.on(l.get(k).name))
                    .setNegativeButton(L.t("Назад"), (d2, k) -> pickBinding(a, done)).show();
        }).setNegativeButton(L.t("Отмена"), null).show();
    }

    /** element settings; applied: after OK (the element was changed in place), deleted: "Удалить" */
    static void element(final Activity a, final El orig, final Runnable applied, final Runnable deleted) {
        final El e = orig.copy();
        final LinearLayout box = new LinearLayout(a); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(px(a, 20), px(a, 6), px(a, 20), px(a, 6));
        box.addView(head(a, L.t("Тип")));
        final Spinner type = spinner(a, TYPE_L, idx(CtlProfile.TYPES, e.type)); box.addView(type);
        final TextView shapeH = head(a, L.t("Форма")); box.addView(shapeH);
        final Spinner shape = spinner(a, SHAPE_L, idx(CtlProfile.SHAPES, e.shape)); box.addView(shape);
        final TextView bindH = head(a, ""); box.addView(bindH);
        final LinearLayout binds = new LinearLayout(a); binds.setOrientation(LinearLayout.VERTICAL); box.addView(binds);
        /* range button */
        final LinearLayout rbox = new LinearLayout(a); rbox.setOrientation(LinearLayout.VERTICAL); box.addView(rbox);
        rbox.addView(head(a, L.t("Клавиши ряда")));
        final Spinner range = spinner(a, RANGE_L, idx(CtlProfile.RANGES, e.range)); rbox.addView(range);
        final TextView cellsL = head(a, ""); rbox.addView(cellsL);
        final SeekBar cells = new SeekBar(a); cells.setMax(10); cells.setProgress(e.cells - 2); rbox.addView(cells);
        final Switch vert = new Switch(a); vert.setText(L.t("Вертикально")); vert.setChecked(e.orient == 1); rbox.addView(vert);
        /* size, toggle, label */
        final TextView scaleL = head(a, ""); box.addView(scaleL);
        final SeekBar scale = new SeekBar(a); scale.setMax(74); scale.setProgress(Math.round((e.scale - 0.3f) / 0.05f)); box.addView(scale);
        final Switch toggle = new Switch(a); toggle.setText(L.t("Переключатель (нажал — держится, нажал ещё раз — отпущено)")); toggle.setChecked(e.toggle);
        toggle.setPadding(0, px(a, 8), 0, 0); box.addView(toggle);
        box.addView(head(a, L.t("Подпись (пусто — по назначенной клавише)")));
        final EditText text = new EditText(a); text.setSingleLine(true); text.setText(e.text); text.setInputType(InputType.TYPE_CLASS_TEXT); box.addView(text);

        final Runnable[] rebuild = new Runnable[1];
        rebuild[0] = () -> {
            boolean button = e.is("BUTTON"), rangeB = e.is("RANGE_BUTTON");
            shapeH.setVisibility(button ? View.VISIBLE : View.GONE); shape.setVisibility(button ? View.VISIBLE : View.GONE);
            toggle.setVisibility(button ? View.VISIBLE : View.GONE); rbox.setVisibility(rangeB ? View.VISIBLE : View.GONE);
            bindH.setVisibility(rangeB ? View.GONE : View.VISIBLE);
            bindH.setText(button ? L.t("Назначение (до 4 клавиш — нажимаются вместе)") : L.t("Назначение по направлениям"));
            binds.removeAllViews();
            if (rangeB) return;
            for (int i = 0; i < 4; i++) {
                final int k = i;
                LinearLayout row = new LinearLayout(a); row.setOrientation(LinearLayout.HORIZONTAL);
                TextView l = new TextView(a); l.setTextSize(14);
                l.setText(button ? (i == 0 ? L.t("Клавиша") : "+ " + (i + 1)) : L.t(DIR_L[i]));
                row.addView(l, new LinearLayout.LayoutParams(px(a, 90), ViewGroup.LayoutParams.WRAP_CONTENT));
                final Button b = btn(a, Bind.of(e.bind[i]).desc(), null);
                b.setOnClickListener(v -> pickBinding(a, n -> { e.bind[k] = n; b.setText(Bind.of(n).desc()); }));
                row.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                row.setGravity(android.view.Gravity.CENTER_VERTICAL);
                binds.addView(row);
            }
            if (!button) {
                TextView l = new TextView(a); l.setText(L.t("Быстро назначить:")); l.setTextSize(13); l.setTextColor(Color.GRAY); binds.addView(l);
                LinearLayout row = null;
                for (int p = 0; p < DIR_PRESETS.length; p++) {
                    if (p % 3 == 0) { row = new LinearLayout(a); row.setOrientation(LinearLayout.HORIZONTAL); binds.addView(row); }
                    final String[] pr = DIR_PRESETS[p];
                    Button b = btn(a, L.t(DIR_PRESET_L[p]), v -> { System.arraycopy(pr, 0, e.bind, 0, 4); rebuild[0].run(); });
                    b.setTextSize(12);
                    row.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                }
            }
        };
        final Runnable labels = () -> {
            scaleL.setText(L.t("Размер: ") + Math.round(e.scale * 100) + " %");
            cellsL.setText(L.t("Видно клавиш: ") + e.cells);
        };
        onSel(type, i -> {
            String nt = CtlProfile.TYPES[i]; if (nt.equals(e.type)) return;
            boolean empty = true; for (String b : e.bind) if (!"NONE".equals(b)) empty = false;
            e.type = nt;
            if (empty && nt.equals("STICK")) System.arraycopy(DIR_PRESETS[0], 0, e.bind, 0, 4);
            if (empty && nt.equals("D_PAD")) System.arraycopy(DIR_PRESETS[1], 0, e.bind, 0, 4);
            rebuild[0].run();
        });
        onSel(shape, i -> e.shape = CtlProfile.SHAPES[i]);
        onSel(range, i -> e.range = CtlProfile.RANGES[i]);
        cells.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int p, boolean u) { e.cells = p + 2; labels.run(); }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        });
        scale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int p, boolean u) { e.scale = 0.3f + p * 0.05f; labels.run(); }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        });
        rebuild[0].run(); labels.run();
        ScrollView sv = new ScrollView(a); sv.addView(box);
        new AlertDialog.Builder(a).setTitle(L.t("Элемент управления")).setView(sv)
                .setPositiveButton("OK", (d, w) -> {
                    orig.type = e.type; orig.shape = e.shape; orig.bind = e.bind.clone(); orig.scale = e.scale; orig.toggle = toggle.isChecked() && e.is("BUTTON");
                    orig.text = text.getText().toString().trim(); orig.range = e.range; orig.cells = e.cells; orig.orient = vert.isChecked() ? 1 : 0;
                    orig.latched = false; orig.scroll = 0; orig.resolve();
                    applied.run();
                })
                .setNeutralButton(L.t("Удалить"), (d, w) -> deleted.run())
                .setNegativeButton(L.t("Отмена"), null).show();
    }

    /** profile manager; changed: after the active profile or its content changed; edit: starts the in-game editor (null in the launcher) */
    static void profiles(final Activity a, final Runnable changed, final Runnable edit) {
        final ArrayList<CtlProfile> list = CtlProfile.list(a);
        final CtlProfile act = CtlProfile.active(a);
        final LinearLayout box = new LinearLayout(a); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(px(a, 20), px(a, 6), px(a, 20), px(a, 6));
        final AlertDialog[] dlg = new AlertDialog[1];
        final Runnable reopen = () -> { if (dlg[0] != null) dlg[0].dismiss(); if (changed != null) changed.run(); profiles(a, changed, edit); };
        final RadioGroup rg = new RadioGroup(a);
        for (int i = 0; i < list.size(); i++) {
            CtlProfile p = list.get(i);
            RadioButton rb = new RadioButton(a); rb.setId(View.generateViewId()); rb.setTag(p.file);
            rb.setText(p.name + "  (" + p.els.size() + ")" + (p.builtin ? L.t(" — встроенный") : ""));
            rg.addView(rb); if (p.file.equals(act.file)) rb.setChecked(true);
        }
        rg.setOnCheckedChangeListener((g, id) -> {
            View rb = g.findViewById(id); if (rb == null) return;
            CtlProfile p = CtlProfile.load(a, (String) rb.getTag()); if (p == null) return;
            CtlProfile.setActive(a, p); reopen.run();
        });
        box.addView(rg);
        /* cursor speed of the active profile (Winlator "cursorSpeed": trackpads, mouse-motion bindings) */
        final TextView csL = head(a, ""); box.addView(csL);
        final SeekBar cs = new SeekBar(a); cs.setMax(49); cs.setProgress(Math.max(0, Math.min(49, Math.round(act.cursorSpeed * 10) - 1))); box.addView(cs);
        csL.setText(L.t("Скорость курсора (тачпад, «движение мыши»): ") + String.format(Locale.ROOT, "%.1f", act.cursorSpeed));
        cs.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int p, boolean u) { if (!u) return; act.cursorSpeed = (p + 1) / 10f; csL.setText(L.t("Скорость курсора (тачпад, «движение мыши»): ") + String.format(Locale.ROOT, "%.1f", act.cursorSpeed)); }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) { CtlProfile.save(a, act); if (changed != null) changed.run(); }
        });
        LinearLayout col = new LinearLayout(a); col.setOrientation(LinearLayout.VERTICAL); col.setPadding(0, px(a, 8), 0, 0);
        if (edit != null) col.addView(btn(a, L.t("Редактировать раскладку на экране"), v -> { if (dlg[0] != null) dlg[0].dismiss(); edit.run(); }));
        col.addView(btn(a, L.t("Новый профиль (пустой)"), v -> askName(a, L.t("Новый профиль"), L.t("Новый профиль"), n -> {
            CtlProfile p = new CtlProfile(); p.name = n; CtlProfile.save(a, p); CtlProfile.setActive(a, p); reopen.run();
        })));
        col.addView(btn(a, L.t("Копия выбранного"), v -> askName(a, L.t("Копия профиля"), act.name + " 2", n -> {
            CtlProfile p = act.copy(n); CtlProfile.save(a, p); CtlProfile.setActive(a, p); reopen.run();
        })));
        col.addView(btn(a, L.t("Переименовать"), v -> askName(a, L.t("Переименовать"), act.name, n -> { act.name = n; CtlProfile.save(a, act); reopen.run(); })));
        col.addView(btn(a, L.t("Удалить"), v -> new AlertDialog.Builder(a).setTitle(L.t("Удалить профиль?")).setMessage(act.name)
                .setPositiveButton(L.t("Удалить"), (d, w) -> { CtlProfile.delete(a, act); reopen.run(); }).setNegativeButton(L.t("Отмена"), null).show()));
        col.addView(btn(a, L.t("Импорт из файла .icp (Winlator)…"), v -> CtlProfile.pickImport(a, reopen)));
        col.addView(btn(a, L.t("Экспорт в файл .icp…"), v -> CtlProfile.pickExport(a, act)));
        col.addView(btn(a, L.t("Вернуть встроенные профили"), v -> { CtlProfile.seed(a, true); reopen.run(); }));
        box.addView(col);
        TextView info = new TextView(a); info.setTextSize(12); info.setTextColor(Color.GRAY); info.setPadding(0, px(a, 8), 0, 0);
        info.setText(L.t("Профили совместимы с Winlator: файлы .icp из Winlator импортируются как есть (кнопки, стики, крестовины, тачпады, ряды клавиш), экспорт открывается в Winlator. ")
                + (edit != null ? L.t("В редакторе: «Добавить», перетаскивание, два пальца — размер, повторное касание — настройки элемента.")
                                : L.t("Расставлять элементы — в игре: долгое нажатие на ⊕ → «Профили своей раскладки» → «Редактировать раскладку на экране».")));
        box.addView(info);
        ScrollView sv = new ScrollView(a); sv.addView(box);
        dlg[0] = new AlertDialog.Builder(a).setTitle(L.t("Профили своей раскладки")).setView(sv).setPositiveButton(L.t("Готово"), null).create();
        dlg[0].show();
    }
    private static void askName(Activity a, String title, String def, final Pick done) {
        final EditText et = new EditText(a); et.setSingleLine(true); et.setText(def); et.setSelectAllOnFocus(true);
        LinearLayout f = new LinearLayout(a); f.setPadding(px(a, 20), px(a, 4), px(a, 20), 0); f.addView(et, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        new AlertDialog.Builder(a).setTitle(title).setView(f).setPositiveButton("OK", (d, w) -> {
            String n = et.getText().toString().trim(); if (!n.isEmpty()) done.on(n);
        }).setNegativeButton(L.t("Отмена"), null).show();
    }
}
