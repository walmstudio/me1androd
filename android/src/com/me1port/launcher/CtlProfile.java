package com.me1port.launcher;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;

/** v1.2: a profile of the custom layout = a Winlator input-control profile (.icp, JSON):
 * { id, name, cursorSpeed, elements: [ { type BUTTON|D_PAD|STICK|TRACKPAD|RANGE_BUTTON, shape CIRCLE|RECT|ROUND_RECT|SQUARE,
 * bindings [4 names, Bind], scale, x, y (centre, fractions of the screen), toggleSwitch, text, iconId,
 * range FROM_A_TO_Z|FROM_0_TO_9|FROM_F1_TO_F12|FROM_NP0_TO_NP9, orientation 0|1, bindingCount (visible cells) } ] }.
 * Files: <files>/controls/*.icp (shared by the launcher and the game process); active one: "profile" in
 * ControlsSettings.prefs. Built-in profiles (assets/controls) are copied there on first use as builtin_<name>.icp.
 * Import / export: the system file picker (REQ_IMPORT / REQ_EXPORT, onResult from the activity). */
final class CtlProfile {
    static final String[] TYPES = { "BUTTON", "D_PAD", "STICK", "TRACKPAD", "RANGE_BUTTON" };
    static final String[] SHAPES = { "CIRCLE", "RECT", "ROUND_RECT", "SQUARE" };
    static final String[] RANGES = { "FROM_0_TO_9", "FROM_F1_TO_F12", "FROM_A_TO_Z", "FROM_NP0_TO_NP9" };

    static final class El {
        String type = "BUTTON", shape = "CIRCLE", text = "", range = "FROM_0_TO_9";
        String[] bind = { "NONE", "NONE", "NONE", "NONE" };
        float scale = 1f, x = 0.5f, y = 0.5f; boolean toggle, zone, hidden; int iconId, orient, cells = 4, color; String tag = "";
        /* runtime state (CustomControls) */
        float cx, cy, hw, hh, nx, ny, scroll, scroll0, dx0, dy0, lx, ly, mvx, mvy;
        int ptr = -1, cell = -1, dirs; boolean latched, pressed, moved, cellDown; long t0; Runnable task;
        Bind[] res = new Bind[4];
        void resolve() { for (int i = 0; i < 4; i++) res[i] = Bind.of(bind[i]); }
        boolean is(String t) { return type.equals(t); }
        El copy() {
            El e = new El(); e.type = type; e.shape = shape; e.text = text; e.range = range; e.bind = bind.clone();
            e.scale = scale; e.x = x; e.y = y; e.toggle = toggle; e.iconId = iconId; e.orient = orient; e.cells = cells; e.zone = zone; e.hidden = hidden; e.color = color; e.tag = tag;
            e.resolve();
            return e;
        }
        /** keys of a RANGE_BUTTON in display order */
        String[] rangeKeys() {
            String[] r;
            switch (range) {
            case "FROM_F1_TO_F12": r = new String[12]; for (int i = 0; i < 12; i++) r[i] = "KEY_F" + (i + 1); return r;
            case "FROM_A_TO_Z": r = new String[26]; for (int i = 0; i < 26; i++) r[i] = "KEY_" + (char) ('A' + i); return r;
            case "FROM_NP0_TO_NP9": r = new String[10]; for (int i = 0; i < 10; i++) r[i] = "KEY_KP_" + i; return r;
            default: r = new String[10]; for (int i = 0; i < 10; i++) r[i] = "KEY_" + ((i + 1) % 10); return r;   /* 1..9, 0 as on the keyboard */
            }
        }
        JSONObject json() throws Exception {
            JSONObject o = new JSONObject();
            o.put("type", type); o.put("shape", shape);
            JSONArray b = new JSONArray(); for (String s : bind) b.put(s); o.put("bindings", b);
            o.put("scale", (double) scale); o.put("x", (double) x); o.put("y", (double) y);
            o.put("toggleSwitch", toggle); o.put("text", text); o.put("iconId", iconId);
            if (zone) o.put("zone", true); if (hidden) o.put("hidden", true); if (color != 0) o.put("color", color); if (!tag.isEmpty()) o.put("tag", tag);
            if (is("RANGE_BUTTON")) { o.put("range", range); if (orient != 0) o.put("orientation", orient); o.put("bindingCount", cells); }
            return o;
        }
        static El parse(JSONObject o) {
            El e = new El();
            e.type = pick(o.optString("type", "BUTTON"), TYPES, "BUTTON"); e.shape = pick(o.optString("shape", "CIRCLE"), SHAPES, "CIRCLE");
            JSONArray b = o.optJSONArray("bindings");
            if (b != null) for (int i = 0; i < 4 && i < b.length(); i++) e.bind[i] = b.optString(i, "NONE");
            e.scale = clamp((float) o.optDouble("scale", 1), 0.2f, 4f);
            e.x = clamp((float) o.optDouble("x", 0.5), 0, 1); e.y = clamp((float) o.optDouble("y", 0.5), 0, 1);
            e.toggle = o.optBoolean("toggleSwitch", false); e.text = o.optString("text", ""); e.iconId = o.optInt("iconId", 0);
            e.range = pick(o.optString("range", "FROM_0_TO_9"), RANGES, "FROM_0_TO_9"); e.orient = o.optInt("orientation", 0) != 0 ? 1 : 0;
            e.cells = Math.max(2, Math.min(12, o.optInt("bindingCount", 4)));
            e.zone = o.optBoolean("zone", false); e.hidden = o.optBoolean("hidden", false); e.color = o.optInt("color", 0); e.tag = o.optString("tag", "");
            e.resolve();
            return e;
        }
    }

    int id; String name = "", file; float cursorSpeed = 1f; boolean builtin, slide;
    final ArrayList<El> els = new ArrayList<>();

    static CtlProfile parse(String json) throws Exception {
        JSONObject o = new JSONObject(json);
        CtlProfile p = new CtlProfile();
        p.id = o.optInt("id", 0); p.name = o.optString("name", "").trim(); p.cursorSpeed = clamp((float) o.optDouble("cursorSpeed", 1), 0.1f, 5f);
        p.slide = o.optBoolean("slide", false);
        JSONArray a = o.optJSONArray("elements");
        if (a != null) for (int i = 0; i < a.length(); i++) { JSONObject e = a.optJSONObject(i); if (e != null) p.els.add(El.parse(e)); }
        if (p.name.isEmpty()) p.name = L.t("Без названия");
        return p;
    }
    String json() {
        try {
            JSONObject o = new JSONObject();
            o.put("id", id); o.put("name", name); o.put("cursorSpeed", (double) cursorSpeed); if (slide) o.put("slide", true);
            JSONArray a = new JSONArray(); for (El e : els) a.put(e.json()); o.put("elements", a);
            return o.toString();
        } catch (Exception e) { return "{}"; }
    }

    /* ---- storage ---- */
    static File dir(Context c) { File d = new File(c.getFilesDir(), "controls"); d.mkdirs(); return d; }
    private static String read(InputStream in) throws Exception {
        ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] b = new byte[16384]; int n;
        while ((n = in.read(b)) > 0) bo.write(b, 0, n);
        return new String(bo.toByteArray(), "UTF-8");
    }
    private static void write(File f, String s) throws Exception {
        File t = new File(f.getPath() + ".tmp");
        try (OutputStream o = new FileOutputStream(t)) { o.write(s.getBytes("UTF-8")); }
        if (!t.renameTo(f)) throw new Exception("rename " + f);
    }
    /** copy the built-in profiles (assets/controls/*.icp) that are missing; all of them again with force */
    static void seed(Context c, boolean force) {
        try {
            String[] as = c.getAssets().list("controls");
            if (as == null) return;
            for (String a : as) {
                if (!a.endsWith(".icp")) continue;
                File f = new File(dir(c), "builtin_" + a);
                if (f.exists() && !force) continue;
                if (!force && ControlsSettings.prefs(c).getBoolean("seeded_" + a, false)) continue;   /* deleted by the user */
                try (InputStream in = c.getAssets().open("controls/" + a)) { write(f, read(in)); }
                ControlsSettings.prefs(c).edit().putBoolean("seeded_" + a, true).apply();
            }
        } catch (Exception ignored) {}
    }
    static void seedOne(Context c, String file) {
        if (file == null || !file.startsWith("builtin_")) return;
        try (InputStream in = c.getAssets().open("controls/" + file.substring(8))) { write(new File(dir(c), file), read(in)); }
        catch (Exception ignored) {}
    }
    private static void upgradeMass(Context c) {
        android.content.SharedPreferences prefs = ControlsSettings.prefs(c);
        if (prefs.getBoolean("mass_keys_v122", false)) return;
        CtlProfile old = load(c, "builtin_mass_effect.icp");
        if (old != null) try (InputStream in = c.getAssets().open("controls/mass_effect.icp")) {
            CtlProfile fresh = parse(read(in));
            for (String key : new String[]{"KEY_G", "KEY_TAB", "KEY_J", "SPECIAL_KEYBOARD"}) {
                boolean exists = false; for (El e : old.els) for (String b : e.bind) if (key.equals(b)) exists = true;
                if (!exists) for (El e : fresh.els) if (key.equals(e.bind[0])) { old.els.add(e.copy()); break; }
            }
            if (!save(c, old)) return;
        } catch (Exception e) { return; }
        prefs.edit().putBoolean("mass_keys_v122", true).apply();
    }
    static ArrayList<CtlProfile> list(Context c) {
        seed(c, false);
        ArrayList<CtlProfile> r = new ArrayList<>();
        File[] fs = dir(c).listFiles();
        if (fs != null) for (File f : fs) {
            if (!f.getName().endsWith(".icp") || f.getName().startsWith("layout")) continue;
            try (InputStream in = new FileInputStream(f)) { CtlProfile p = parse(read(in)); p.file = f.getName(); p.builtin = p.file.startsWith("builtin_"); r.add(p); }
            catch (Exception ignored) {}
        }
        Collections.sort(r, (a, b) -> a.name.compareToIgnoreCase(b.name));
        return r;
    }
    static CtlProfile load(Context c, String file) {
        if (file == null) return null;
        File f = new File(dir(c), file);
        try (InputStream in = new FileInputStream(f)) { CtlProfile p = parse(read(in)); p.file = file; p.builtin = file.startsWith("builtin_"); return p; }
        catch (Exception e) { return null; }
    }
    /** the active profile (the first one when none is chosen); never null */
    static CtlProfile active(Context c) {
        seed(c, false); upgradeMass(c);
        android.content.SharedPreferences prefs = ControlsSettings.prefs(c);
        String layout = prefs.getString("layout", "classic");
        if (!layout.equals("custom")) {
            String file = layout.equals("skyrim") ? "layout2.icp" : prefs.getBoolean("xinput", false) ? "layout1x.icp" : "layout1.icp";
            CtlProfile built = load(c, file);
            if (built == null) try (InputStream in = c.getAssets().open("layouts/" + file)) {
                built = parse(read(in)); built.file = file;
                java.util.HashSet<String> hid = ControlsSettings.hidden(prefs);
                for (El el : built.els) {
                    String tag = el.tag; if (tag.isEmpty()) continue;
                    String key = layout.equals("skyrim") ? "s_" + tag : "p_" + tag;
                    String pos = prefs.getString(key, null);
                    if (pos != null) try { String[] xy = pos.split(","); el.x = clamp(Float.parseFloat(xy[0]), 0, 1); el.y = clamp(Float.parseFloat(xy[1]), 0, 1); } catch (Exception ignored) {}
                    el.hidden = hid.contains(layout.equals("skyrim") ? "s:" + tag : tag);
                }
                save(c, built);
            } catch (Exception ignored) {}
            if (built != null) return built;
        }
        CtlProfile p = load(c, prefs.getString("profile", null));
        if (p != null) return p;
        ArrayList<CtlProfile> l = list(c);
        if (!l.isEmpty()) { p = l.get(0); setActive(c, p); return p; }
        p = new CtlProfile(); p.name = L.t("Новый профиль"); save(c, p); setActive(c, p);
        return p;
    }
    static void setActive(Context c, CtlProfile p) { ControlsSettings.prefs(c).edit().putString("profile", p.file).apply(); }
    static boolean save(Context c, CtlProfile p) {
        if (p.file == null) {
            int max = 0; for (CtlProfile q : list(c)) max = Math.max(max, q.id);
            if (p.id <= 0 || exists(c, p.id)) p.id = max + 1;
            p.file = "p" + p.id + "_" + System.currentTimeMillis() % 100000 + ".icp";
        }
        try { write(new File(dir(c), p.file), p.json()); return true; } catch (Exception e) { return false; }
    }
    private static boolean exists(Context c, int id) { for (CtlProfile q : list(c)) if (q.id == id) return true; return false; }
    static void delete(Context c, CtlProfile p) {
        if (p.file != null) new File(dir(c), p.file).delete();
        if (p.file != null && p.file.equals(ControlsSettings.prefs(c).getString("profile", null))) ControlsSettings.prefs(c).edit().remove("profile").apply();
    }
    CtlProfile copy(String newName) {
        CtlProfile p = new CtlProfile(); p.id = 0; p.name = newName; p.cursorSpeed = cursorSpeed; p.slide = slide;
        for (El e : els) p.els.add(e.copy());
        return p;
    }

    /* ---- import / export through the system file picker ---- */
    static final int REQ_IMPORT = 0x1C01, REQ_EXPORT = 0x1C02;
    private static Runnable s_after; private static String s_export;
    static void pickImport(Activity a, Runnable after) {
        s_after = after;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*");
        try { a.startActivityForResult(i, REQ_IMPORT); } catch (Exception e) { toast(a, L.t("Нет приложения для выбора файлов")); }
    }
    static void pickExport(Activity a, CtlProfile p) {
        s_export = p.file;
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/octet-stream");
        i.putExtra(Intent.EXTRA_TITLE, p.name.replaceAll("[\\\\/:*?\"<>|]", "_") + ".icp");
        try { a.startActivityForResult(i, REQ_EXPORT); } catch (Exception e) { toast(a, L.t("Нет приложения для выбора файлов")); }
    }
    /** from Activity.onActivityResult: true when the request was ours */
    static boolean onResult(Activity a, int req, int res, Intent data) {
        if (req != REQ_IMPORT && req != REQ_EXPORT) return false;
        Uri u = data != null ? data.getData() : null;
        if (res != Activity.RESULT_OK || u == null) return true;
        if (req == REQ_IMPORT) {
            try (InputStream in = a.getContentResolver().openInputStream(u)) {
                CtlProfile p = parse(read(in));
                if (p.els.isEmpty()) throw new Exception("no elements");
                p.file = null; save(a, p); setActive(a, p);
                toast(a, L.t("Профиль импортирован: ") + p.name + " (" + p.els.size() + ")");
                if (s_after != null) s_after.run();
            } catch (Exception e) { toast(a, L.t("Не удалось прочитать профиль: ") + e.getMessage()); }
        } else {
            CtlProfile p = load(a, s_export);
            try (OutputStream o = a.getContentResolver().openOutputStream(u)) {
                if (p == null || o == null) throw new Exception("?");
                o.write(p.json().getBytes("UTF-8")); toast(a, L.t("Профиль сохранён в файл"));
            } catch (Exception e) { toast(a, L.t("Не удалось сохранить: ") + e.getMessage()); }
        }
        return true;
    }

    static String pick(String v, String[] all, String def) { for (String s : all) if (s.equals(v)) return v; return def; }
    static float clamp(float v, float lo, float hi) { return Float.isNaN(v) ? lo : v < lo ? lo : v > hi ? hi : v; }
    static void toast(Context c, String s) { Toast.makeText(c, s, Toast.LENGTH_LONG).show(); }
    static String fmt(float v) { return String.format(Locale.ROOT, "%.2f", v); }
}
