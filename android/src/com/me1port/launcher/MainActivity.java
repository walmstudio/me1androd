package com.me1port.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.net.Uri;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Launcher with the settings screen (replacement for MassEffectConfig.exe). UI is built in code: no resources needed. */
public class MainActivity extends Activity {
    private static final int ACCENT = Color.rgb(0x4F, 0xA3, 0xE0);
    private static final int REQ_STORAGE = 1;
    private static final String READ = "android.permission.READ_EXTERNAL_STORAGE", WRITE = "android.permission.WRITE_EXTERNAL_STORAGE";
    private File gameDir, docsDir, sdRoot;
    private GameLocator loc;
    private SharedPreferences prefs;
    private JSONArray settings, presets;
    private final Map<String, String> values = new HashMap<>();
    private final Map<String, Runnable> refreshers = new HashMap<>();
    private TextView statusView;
    private boolean updating;
    private DeviceInfo dev;
    private LinearLayout gpuBox;
    private Tour tour;
    private TextView gmHintView;
    private boolean gamePending;
    private boolean firstRunDialog, permissionPending;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("me1port", MODE_PRIVATE);
        L.init(this);
        loadSchema();
        if (!hasStorage()) { prefs.edit().putBoolean("asked", true).apply(); permissionPending = true; requestPermissions(new String[] { READ, WRITE }, REQ_STORAGE); }
        relocate(true);
    }

    /** settings / presets from the native schema, translated into the launcher language (v1.0) */
    private void loadSchema() {
        try {
            JSONObject s = new JSONObject(Native.schema()); L.schema(s);
            settings = s.getJSONArray("settings"); presets = s.getJSONArray("presets");
        } catch (Exception e) { throw new RuntimeException(e); }
    }
    /** v1.0: launcher language (= the game's text languages); optionally the game text language too */
    private void chooseLanguage() {
        final int was = L.cur();
        new AlertDialog.Builder(this).setTitle("🌐 " + L.t("Язык интерфейса"))
                .setSingleChoiceItems(L.NAMES, was, (d, i) -> {
                    d.dismiss();
                    if (i == was) return;
                    L.set(this, i);
                    String game = values.get("game.language");
                    if (L.GAME[i].equals(game)) { loadSchema(); relocate(true); return; }
                    new AlertDialog.Builder(this).setTitle(L.t("Язык текста в игре"))
                            .setMessage(L.t("Переключить и язык текста в игре на ") + L.NAMES[i] + L.t("? Нужны файлы локализации этого языка в папке игры."))
                            .setPositiveButton(L.t("Да"), (d2, w) -> {
                                String err = L.tx(Native.set("game.language", L.GAME[i]));
                                String r = err.isEmpty() ? L.tx(Native.save()) : "ERR\n" + err;
                                if (!r.startsWith("OK")) toast(r.substring(r.indexOf('\n') + 1));
                                loadSchema(); relocate(true);
                            })
                            .setNegativeButton(L.t("Нет"), (d2, w) -> { loadSchema(); relocate(true); })
                            .setOnCancelListener(d2 -> { loadSchema(); relocate(true); }).show();
                })
                .setNegativeButton(L.t("Отмена"), null).show();
    }
    private String version() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return "?"; }
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        if (code == REQ_STORAGE) { permissionPending = false; relocate(true); offerTour(); }
    }
    /** v1.2: import / export of custom-layout profiles (CtlProfile, system file picker) */
    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (CtlProfile.onResult(this, req, res, data)) return;
        super.onActivityResult(req, res, data);
    }
    /** files may have been copied / permission granted while we were in the background */
    @Override protected void onResume() {
        super.onResume();
        if (loc != null && !loc.found) relocate(false);
        else if (gpuBox != null) buildGpu();
        if (gamePending && loc != null) { gamePending = false; String d = Native.refreshClean(); if (!d.isEmpty()) { pull(d); refreshAll(); } }
        if (gmHintView != null) { String h = gmHint(); gmHintView.setText(h); gmHintView.setVisibility(h.isEmpty() ? View.GONE : View.VISIBLE); }   /* drivers may have been copied into GameExDriver meanwhile */
    }
    private boolean hasStorage() { return checkSelfPermission(READ) == PackageManager.PERMISSION_GRANTED; }

    /** storage roots: /sdcard first, then SD cards (root of each getExternalFilesDirs entry) */
    private List<File> storageRoots() {
        List<File> r = new ArrayList<>();
        sdRoot = Environment.getExternalStorageDirectory(); r.add(sdRoot);
        for (File f : getExternalFilesDirs(null)) {
            if (f == null) continue;
            String p = f.getAbsolutePath(); int i = p.indexOf("/Android/data/");
            if (i > 0) { File root = new File(p.substring(0, i)); if (!root.equals(sdRoot) && !r.contains(root)) r.add(root); }
        }
        return r;
    }
    /** detect the game folder; rebuild the screen if it (or the documents folder) changed */
    private void relocate(boolean force) {
        File priv = getExternalFilesDir(null); if (priv == null) priv = getFilesDir();
        GameLocator l = GameLocator.locate(storageRoots(), priv, prefs.getString("gameDir", null));
        boolean changed = loc == null || !l.gameDir.equals(gameDir) || !l.docsDir.equals(docsDir) || l.found != loc.found;
        loc = l; gameDir = l.gameDir; docsDir = l.docsDir;
        if (l.migrated != null) toast(l.migrated);
        if (!force && !changed) { updateStatus(); return; }
        reload();
        refreshers.clear();
        setContentView(buildUi());
        updateStatus();
        autoFirstRun();
        if (hasStorage()) offerTour();
    }
    private void offerTour() { if (!permissionPending && !firstRunDialog && !prefs.getBoolean("tourShown", false) && tour != null) { prefs.edit().putBoolean("tourShown", true).apply(); getWindow().getDecorView().post(() -> { if (!isFinishing() && tour != null) tour.show(); }); } }
    /** v0.9.7: a fresh install (no settings saved yet) gets the automatic preset once */
    private void autoFirstRun() {
        if (prefs.getBoolean("autoDone", false) || dev == null) return;
        prefs.edit().putBoolean("autoDone", true).apply();
        if (new File(docsDir, "BioWare/Mass Effect/me1port.ini").exists()) return;   /* the user already has settings */
        String r = dev.classify(true);
        if (r.startsWith("-1")) return;
        String s = L.tx(Native.save());
        pull(Native.dump()); refreshAll();
        firstRunDialog = true;
        new AlertDialog.Builder(this).setTitle(L.t("Настройки подобраны автоматически"))
                .setMessage(r.substring(r.indexOf('\n') + 1) + (s.startsWith("OK") ? "" : "\n\n" + s.substring(s.indexOf('\n') + 1))
                        + L.t("\n\nИзменить можно ниже (пресеты и отдельные настройки).")).setPositiveButton("OK", null).setOnDismissListener(d -> { firstRunDialog = false; offerTour(); }).show();
    }

    /* ---------------- controls profile + mods (v0.9.7) ---------------- */
    /** mods: <game>/Mods/<name> with "Mass Effect"/BioGame/DLC and/or Config inside */
    private File modsDir() { return new File(gameDir, "Mods"); }
    private List<File> findMods() {
        List<File> r = new ArrayList<>();
        File[] fs = modsDir().listFiles();
        if (fs != null) for (File f : fs) if (f.isDirectory() && isMod(f)) r.add(f);
        java.util.Collections.sort(r, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        return r;
    }
    private static final String[] MOD_DIRS = { "Mass Effect", "MassEffect", "Mass Effect 1", "BioGame", "DLC", "Binaries", "Config" };
    private static boolean hasModDir(File f) { for (String s : MOD_DIRS) if (new File(f, s).isDirectory()) return true; return false; }
    private static boolean isMod(File f) { return hasModDir(modRoot(f)); }
    /** the folder that holds Config / "Mass Effect" (archives often add one more level) */
    private static File modRoot(File f) {
        if (hasModDir(f)) return f;
        File[] sub = f.listFiles();   /* "ME1Controller/ME1Controller/Config": archives often add one more level */
        if (sub != null) for (File s : sub) if (s.isDirectory() && hasModDir(s)) return s;
        return f;
    }
    /** a controller mod: its Config/BIOInput.ini binds XboxTypeS_* keys */
    private static boolean isPadMod(File mod) {
        File ini = new File(modRoot(mod), "Config/BIOInput.ini");
        if (!ini.exists()) { File[] c = new File(modRoot(mod), "Config").listFiles(); if (c != null) for (File x : c) if (x.getName().equalsIgnoreCase("BIOInput.ini")) ini = x; }
        try {   /* v1.1.4: raw bytes without NULs - the ini may be UTF-16, FileReader missed that */
            byte[] b = java.nio.file.Files.readAllBytes(ini.toPath());
            StringBuilder t = new StringBuilder(b.length);
            for (byte x : b) if (x != 0) t.append((char) (x & 0xFF));
            return t.indexOf("XboxTypeS_") >= 0;
        } catch (Exception ignored) { }
        return false;
    }
    /** v1.1.4: the first enabled controller mod or null */
    private String padModName() {
        for (File m : findMods()) if (prefs.getBoolean("mod_" + m.getName(), false) && isPadMod(m)) return m.getName();
        return null;
    }
    /** tells the in-game controls dialog (controls prefs "padMod") which controller mod is on */
    private String syncPadMod() {
        String pm = padModName();
        SharedPreferences cp = ControlsSettings.prefs(this);
        if (pm == null ? cp.contains("padMod") : !pm.equals(cp.getString("padMod", null)))
            (pm == null ? cp.edit().remove("padMod") : cp.edit().putString("padMod", pm)).apply();
        return pm;
    }
    /** v1.1.4: a controller mod + the keyboard/mouse profile = no controls at all (the mod rebinds the game to the Xbox pad);
     *  then != null: called from "Start game", runs after the choice (Back cancels the start) */
    private void padModWarning(final String mod, final Runnable then) {
        final SharedPreferences cp = ControlsSettings.prefs(this);
        final Runnable fin = () -> { relocate(true); if (then != null) then.run(); };
        new AlertDialog.Builder(this).setTitle(L.t("Включён мод для геймпада: ") + mod)
                .setMessage(L.t("Мод переводит управление игры на геймпад Xbox. С профилем «Клавиатура и мышь» не будут работать ни раскладка ПК, ни раскладка как в Skyrim — игра не примет ни касания, ни клавиши. ")
                        + L.t("Отключите мод сами (переключатель в списке модов в разделе «Управление и моды») или верните профиль «Геймпад Xbox / XInput».")
                        + (then != null ? L.t("\n\nИгра запустится после выбора.") : ""))
                .setPositiveButton(L.t("Вернуть геймпад XInput"), (d, w) -> { cp.edit().putBoolean("xinput", true).apply(); fin.run(); })
                .setNegativeButton(L.t("Отключить мод"), (d, w) -> {
                    SharedPreferences.Editor e = prefs.edit();
                    for (File m : findMods()) if (prefs.getBoolean("mod_" + m.getName(), false) && isPadMod(m)) e.putBoolean("mod_" + m.getName(), false);
                    e.apply(); syncPadMod(); toast(L.t("Мод отключён: ") + mod); fin.run();
                })
                .setNeutralButton(L.t("Оставить как есть"), (d, w) -> fin.run())
                .setOnCancelListener(d -> relocate(true))
                .show();
    }
    private List<File> enabledMods() {
        List<File> r = new ArrayList<>();
        for (File m : findMods()) if (prefs.getBoolean("mod_" + m.getName(), false)) r.add(modRoot(m));
        return r;
    }
    private static String pads() {
        StringBuilder b = new StringBuilder();
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
                    || (s & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK)
                b.append(b.length() > 0 ? ", " : "").append(d.getName());
        }
        return b.toString();
    }
    private void buildControls(LinearLayout root) {
        final SharedPreferences cp = ControlsSettings.prefs(this);
        String p = pads();
        root.addView(text(p.isEmpty() ? L.t("Геймпад не подключён (USB/Bluetooth-геймпады определяются автоматически).") : L.t("Геймпад: ") + p, 13, Color.LTGRAY, false));
        final Spinner prof = new Spinner(this);
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new String[] {
                L.t("Клавиатура и мышь (оригинальная игра)"), L.t("Геймпад Xbox / XInput (мод ME1Controller)") });
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        prof.setAdapter(ad); prof.setSelection(cp.getBoolean("xinput", false) ? 1 : 0, false);
        root.addView(prof);
        final TextView warn = text("", 12, Color.rgb(0xF0, 0xB0, 0x40), false);
        prof.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> a, View v, int pos, long l) {
                if ((pos == 1) != cp.getBoolean("xinput", false)) {
                    cp.edit().putBoolean("xinput", pos == 1).apply();
                    String pm = pos == 0 ? padModName() : null;
                    if (pm != null) padModWarning(pm, null); else relocate(true);
                }
            }
            @Override public void onNothingSelected(AdapterView<?> a) {}
        });
        boolean xi = cp.getBoolean("xinput", false);
        String pmName = syncPadMod(); boolean padMod = pmName != null;
        root.addView(text(xi ? L.t("Физический и экранный геймпад (⊕ в игре) работают как контроллер Xbox 360. В оригинальной игре нет ")
                + L.t("привязок для геймпада — нужен мод ME1Controller (включите его ниже). Мод рассчитан на 16:9 и английский текст подсказок.")
                : L.t("Геймпад эмулирует клавиатуру и мышь: левый стик — WASD, правый — камера, RT — огонь, LT — прицел, A — E, B — Q, ")
                + L.t("X — Пробел, Y — F, LB — R, RB — Shift, L3 — Ctrl, R3 — J, Start — Esc, Back — M, крестовина — стрелки. ")
                + L.t("Меню — касанием."), 12, Color.GRAY, false));

        root.addView(text(L.t("Режим экрана:"), 15, Color.LTGRAY, true));
        final Spinner stretch = new Spinner(this);
        ArrayAdapter<String> stAd = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new String[] {
            L.t("По размеру (с пропорциями)"), L.t("Растянуть на весь экран"), L.t("Заполнить экран (обрезать края)") });
        stAd.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        stretch.setAdapter(stAd);
        final String sm = values.get("port.stretch_mode");
        int smIdx = "stretch".equals(sm) ? 1 : "fill".equals(sm) ? 2 : 0;
        stretch.setSelection(smIdx, false);
        root.addView(stretch);
        stretch.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> a, View v, int pos, long l) {
                String val = pos == 1 ? "stretch" : pos == 2 ? "fill" : "fit";
                if (!val.equals(values.get("port.stretch_mode"))) apply("port.stretch_mode", val);
            }
            @Override public void onNothingSelected(AdapterView<?> a) {}
        });

        if (xi && !padMod) { warn.setText(L.t("⚠ Не включён мод с привязками геймпада (Config/BIOInput.ini с XboxTypeS_…) — в игре геймпад работать не будет.")); root.addView(warn); }
        if (!xi && padMod) { warn.setText(L.t("⚠ Включён мод для геймпада: ") + pmName + L.t(" — с профилем «Клавиатура и мышь» в игре не будет работать ни раскладка ПК, ни раскладка как в Skyrim. Выберите «Геймпад Xbox / XInput» или отключите мод ниже.")); root.addView(warn); }
        List<File> mods = findMods();
        root.addView(text(dlcStatus(), 13, Color.LTGRAY, false));
        root.addView(text(L.t("Моды: ") + pretty(modsDir()) + L.t("/<имя мода>/ (внутри — Config и «Mass Effect»; файлы подменяются при запуске, ")
                + L.t("оригинал не меняется)") + (mods.isEmpty() ? L.t(" — модов нет") : ""), 12, Color.GRAY, false));
        for (final File m : mods) {
            final Switch sw = new Switch(this);
            boolean pm = isPadMod(m);
            sw.setText(m.getName() + (pm ? L.t(" (геймпад)") : "")); sw.setTextSize(15); sw.setChecked(prefs.getBoolean("mod_" + m.getName(), false));
            sw.setOnCheckedChangeListener((b, on) -> {
                prefs.edit().putBoolean("mod_" + m.getName(), on).apply();
                if (on && pm && !cp.getBoolean("xinput", false)) { cp.edit().putBoolean("xinput", true).apply(); toast(L.t("Профиль управления: геймпад XInput")); }
                if (!on && pm && cp.getBoolean("xinput", false) && padModName() == null) { cp.edit().putBoolean("xinput", false).apply(); toast(L.t("Профиль управления: клавиатура и мышь")); }
                relocate(true);
            });
            root.addView(sw);
            String dn = modDlcNote(m);
            if (!dn.isEmpty()) { TextView t = text(dn, 12, Color.GRAY, false); t.setPadding(dp(12), 0, 0, dp(4)); root.addView(t); }
        }
    }
    /* ---------------- DLC (v0.9.8) ---------------- */
    private static final String[][] DLCS = { { "DLC_UNC", "Bring Down the Sky" }, { "DLC_Vegas", "Pinnacle Station" } };
    private static File childCI(File dir, String name) {
        File[] fs = dir == null ? null : dir.listFiles();
        if (fs != null) for (File f : fs) if (f.isDirectory() && f.getName().equalsIgnoreCase(name)) return f;
        return null;
    }
    private static String dlcName(String dir) { for (String[] d : DLCS) if (d[0].equalsIgnoreCase(dir)) return d[1]; return dir; }
    private String dlcStatus() {
        File dlc = childCI(gameDir, "DLC");
        StringBuilder b = new StringBuilder(L.t("DLC в папке игры:"));
        for (String[] d : DLCS) b.append(d == DLCS[0] ? " " : " · ").append(d[1]).append(childCI(dlc, d[0]) != null ? " ✓" : L.t(" — нет"));
        return b.toString();
    }
    /** a mod's DLC files (DLC/DLC_Vegas/...) are used only when that DLC is installed - say so instead of failing silently */
    private String modDlcNote(File mod) {
        File r = modRoot(mod), g = null;
        for (String s : new String[] { "Mass Effect", "MassEffect", "Mass Effect 1" }) if (g == null) g = childCI(r, s);
        File md = childCI(g != null ? g : r, "DLC");
        File[] fs = md == null ? null : md.listFiles();
        if (fs == null) return "";
        File base = childCI(gameDir, "DLC");
        List<String> have = new ArrayList<>(), wait = new ArrayList<>();
        for (File f : fs) if (f.isDirectory()) (childCI(base, f.getName()) != null ? have : wait).add(dlcName(f.getName()));
        StringBuilder b = new StringBuilder();
        if (!have.isEmpty()) b.append(L.t("файлы для ")).append(String.join(", ", have)).append(" ✓");
        if (!wait.isEmpty()) b.append(b.length() > 0 ? "; " : "").append(L.t("файлы для ")).append(String.join(", ", wait))
                .append(L.t(" подключатся сами, когда DLC появится в папке игры (")).append(pretty(new File(gameDir, "DLC"))).append(")");
        return b.toString();
    }

    /* ---------------- graphics driver (v0.9.8) ---------------- */
    private List<GpuDrivers.Driver> drivers = new ArrayList<>();
    private void buildGpu() {
        final LinearLayout box = gpuBox; box.removeAllViews();
        drivers = GpuDrivers.scan(gameDir);
        final List<String> ids = new ArrayList<>(), labels = new ArrayList<>();
        ids.add(""); labels.add(L.t("Системный OpenGL ES (по умолчанию)"));
        ids.add("vulkan"); labels.add(L.t("Системный Vulkan — свой рендерер D3D9→Vulkan (новое)"));
        ids.add("zink"); labels.add(L.t("Системный Vulkan + Mesa Zink (эксперимент)"));
        for (GpuDrivers.Driver d : drivers) {
            ids.add("pkg:" + d.id);
            labels.add(d.label() + (d.error != null ? " ✗" : GpuDrivers.compat(d, dev.gpu, Build.VERSION.SDK_INT, Build.VERSION.RELEASE).isEmpty() ? "" : " ⚠"));
        }
        String cur = prefs.getString("gpu_driver", "");
        if (ids.indexOf(cur) < 0) { ids.add(cur); labels.add(cur.substring(cur.indexOf(':') + 1) + L.t(" — нет в папке (будет системный)")); }
        final Spinner sp = new Spinner(this);
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad); sp.setSelection(ids.indexOf(cur), false);
        box.addView(sp);
        final TextView info = text("", 12, Color.GRAY, false), warn = text("", 12, Color.rgb(0xF0, 0xB0, 0x40), false);
        /* v0.10: custom Vulkan drivers run the own D3D9->Vulkan renderer by default; Mesa zink stays as an option */
        final Switch zk = new Switch(this);
        zk.setText(L.t("Через Mesa Zink (OpenGL ES → Vulkan) вместо своего рендерера")); zk.setTextSize(14);
        zk.setChecked("zink".equals(prefs.getString("drv_renderer", "vulkan")));
        zk.setOnCheckedChangeListener((b, on) -> prefs.edit().putString("drv_renderer", on ? "zink" : "vulkan").apply());
        box.addView(warn); box.addView(zk); box.addView(info);
        final Runnable show = () -> {
            String id = prefs.getString("gpu_driver", ""), w = "", i;
            GpuDrivers.Driver d = driver(id);
            zk.setVisibility(id.startsWith("pkg:") && d != null && d.error == null ? View.VISIBLE : View.GONE);
            if (id.isEmpty()) i = L.t("Драйвер устройства — самый быстрый и проверенный вариант.");
            else if (id.equals("vulkan")) i = L.t("Собственный рендерер: команды Direct3D 9 игры сразу идут в Vulkan драйвера устройства, без OpenGL ES и без Zink. ")
                    + L.t("Нужен только Vulkan 1.0 (подходят Mali-G52 и Adreno 6xx); шейдеры собираются в SPIR-V и кэшируются. Кадры выводятся ")
                    + L.t("через swapchain, если его нет — через CPU. Если Vulkan не запустится — будет системный OpenGL ES (строки «[vk]» в логе).");
            else if (id.equals("zink")) i = L.t("Mesa Zink 26.2.6 переводит OpenGL ES игры в Vulkan системного драйвера. Кадры выводятся на экран через CPU — ")
                    + L.t("медленнее системного варианта; для проверки, если с ним есть артефакты. Если Vulkan не запустится — будет системный OpenGL ES.");
            else if (d == null) { w = L.t("Пакета нет в ") + pretty(GpuDrivers.folder(gameDir)) + L.t(" — игра запустится с системным OpenGL ES."); i = ""; }
            else {
                if (d.error != null) w = L.t("✗ Пакет не подходит: ") + d.error;
                else w = GpuDrivers.compat(d, dev.gpu, Build.VERSION.SDK_INT, Build.VERSION.RELEASE);
                i = d.details() + L.t("\nПо умолчанию — свой рендерер D3D9→Vulkan (нужен только Vulkan 1.0, кадры через swapchain или CPU). ")
                        + L.t("С переключателем выше — Mesa Zink 26.2.6 (нужны Vulkan 1.1, VK_KHR_maintenance1 и VK_EXT_robustness2, кадры через CPU). ")
                        + L.t("Если драйвер не загрузится, игра сама перейдёт на системный OpenGL ES; причина — в логе: строки «[vk]», «[gl] … zink check» и «ZINK».");
            }
            warn.setText(w); warn.setVisibility(w.isEmpty() ? View.GONE : View.VISIBLE); info.setText(i);
        };
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> a, View v, int pos, long l) {
                if (!ids.get(pos).equals(prefs.getString("gpu_driver", ""))) { prefs.edit().putString("gpu_driver", ids.get(pos)).apply(); show.run(); }
            }
            @Override public void onNothingSelected(AdapterView<?> a) {}
        });
        show.run();
        box.addView(text(L.t("Свои драйверы: положите .zip в формате AdrenoTools (meta.json + .so: Turnip, сборки Qualcomm, Mali) или ")
                + L.t("папку/.so в ") + pretty(GpuDrivers.folder(gameDir)) + L.t("/ — список обновляется при возврате в лаунчер. Turnip и драйверы ")
                + L.t("Qualcomm работают только на Adreno, драйверы Mali — только на Mali."), 12, Color.GRAY, false));
        box.addView(button(L.t("Обновить список драйверов"), v -> { buildGpu(); toast(L.t("Драйверов в папке: ") + drivers.size()); }));
    }
    private GpuDrivers.Driver driver(String id) {
        for (GpuDrivers.Driver d : drivers) if (("pkg:" + d.id).equals(id)) return d;
        return null;
    }
    private interface Go { void run(Map<String, String> drvEnv); }
    /** environment for the selected driver; custom packages are unpacked to app storage first (Android loads code only from there) */
    private void prepareDriver(final boolean test, final Go go) {
        final String id = prefs.getString("gpu_driver", "");
        final Map<String, String> env = new java.util.LinkedHashMap<>();
        if (id.isEmpty()) { go.run(env); return; }
        final File glc = new File(getFilesDir(), "gl_cache");
        if (test) env.put("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir);   /* the test executable dlopens Mesa/libadrenotools by name */
        if (id.equals("zink") || id.equals("vulkan")) {   /* system Vulkan: Mesa zink or the own Vulkan renderer */
            File c = new File(glc, id); c.mkdirs();
            env.put("ME1_GPU_DRIVER", id); env.put("ME1_GL_CACHE", c.getAbsolutePath()); go.run(env); return;
        }
        drivers = GpuDrivers.scan(gameDir);
        final GpuDrivers.Driver d = driver(id);
        if (d == null || d.error != null) {
            toast((d == null ? L.t("Драйвер ") + id.substring(4) + L.t(" не найден") : L.t("Драйвер не подходит: ") + d.error) + L.t(" — запуск с системным OpenGL ES"));
            go.run(new java.util.LinkedHashMap<>()); return;
        }
        final Runnable install = () -> {
            final AlertDialog dlg = new AlertDialog.Builder(this).setTitle(L.t("Драйвер")).setMessage(L.t("Подготовка ") + d.label() + "…").setCancelable(false).show();
            new Thread(() -> {
                String err = null;
                try {
                    File base = new File(getFilesDir(), "gpu_drivers");
                    File lib = GpuDrivers.install(base, d);
                    GpuDrivers.cleanup(base, drivers);
                    boolean zink = "zink".equals(prefs.getString("drv_renderer", "vulkan"));
                    File c = new File(glc, "drv_" + d.id + (zink ? "_zink" : "")); c.mkdirs();
                    env.put("ME1_GPU_DRIVER", lib.getAbsolutePath()); env.put("ME1_GPU_DRIVER_NAME", d.label()); env.put("ME1_GL_CACHE", c.getAbsolutePath());
                    if (zink) env.put("ME1_GPU_ZINK", "1");
                } catch (Exception e) { err = e.getMessage(); }
                final String fe = err;
                runOnUiThread(() -> {
                    dlg.dismiss();
                    if (fe == null) { go.run(env); return; }
                    new AlertDialog.Builder(this).setTitle(L.t("Драйвер не распакован")).setMessage(fe + L.t("\n\nЗапустить с системным OpenGL ES?"))
                            .setPositiveButton(L.t("Запустить"), (x, y) -> go.run(new java.util.LinkedHashMap<>())).setNegativeButton(L.t("Отмена"), null).show();
                });
            }).start();
        };
        String w = GpuDrivers.compat(d, dev.gpu, Build.VERSION.SDK_INT, Build.VERSION.RELEASE);
        if (w.isEmpty()) { install.run(); return; }
        new AlertDialog.Builder(this).setTitle(d.label()).setMessage(w + L.t("\n\nЕсли драйвер не загрузится, игра сама запустится с системным OpenGL ES."))
                .setPositiveButton(L.t("Всё равно попробовать"), (x, y) -> install.run())
                .setNeutralButton(L.t("Системный OpenGL ES"), (x, y) -> go.run(new java.util.LinkedHashMap<>()))
                .setNegativeButton(L.t("Отмена"), null).show();
    }
    /** /storage/emulated/0/… -> /sdcard/… for display */
    private String pretty(File f) {
        String p = f.getAbsolutePath(), r = sdRoot != null ? sdRoot.getAbsolutePath() : null;
        return r != null && p.startsWith(r) ? "/sdcard" + p.substring(r.length()) : p;
    }

    /** re-read files (start, after save) */
    private void reload() { pull(Native.load(gameDir.getAbsolutePath(), docsDir.getAbsolutePath())); }
    /** current in-memory state incl. unsaved changes (after preset / reset) */
    private void pull(String dump) {
        values.clear();
        for (String line : dump.split("\n")) {
            int eq = line.indexOf('='), tab = line.indexOf('\t');
            if (eq > 0) values.put(line.substring(0, eq), tab > eq ? line.substring(eq + 1, tab) : line.substring(eq + 1));
        }
    }
    private void refreshAll() { updating = true; for (Runnable r : refreshers.values()) r.run(); updating = false; }

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this); t.setText(s); t.setTextSize(sp); t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }
    private Button button(String s, View.OnClickListener l) { Button bt = new Button(this); bt.setText(s); bt.setAllCaps(false); bt.setOnClickListener(l); return bt; }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(12), dp(16), dp(24));
        tour = new Tour(this);
        LinearLayout title = new LinearLayout(this); title.setOrientation(LinearLayout.HORIZONTAL); title.setGravity(Gravity.CENTER_VERTICAL);
        title.addView(text(L.t("Mass Effect — порт для Android"), 22, Color.WHITE, true), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button lang = button("🌐 " + L.CODES[L.cur()].toUpperCase(Locale.ROOT), v -> chooseLanguage());
        lang.setMinWidth(0); lang.setMinimumWidth(0); lang.setPadding(dp(10), 0, dp(10), 0);
        title.addView(lang);
        root.addView(title);
        if (dev == null) dev = DeviceInfo.get(this, prefs);
        String cls = dev.classify(false), rep = cls.substring(cls.indexOf('\n') + 1);
        root.addView(text("v" + version() + " · " + Build.MANUFACTURER + " " + Build.MODEL + " · " + dev.soc + " · Android " + Build.VERSION.RELEASE
                + "\nGPU: " + (dev.gpu.isEmpty() ? "?" : dev.gpu) + (dev.glVersion.isEmpty() ? "" : " (" + dev.glVersion + ")")
                + L.t("\nАвтопресет: ") + rep, 13, Color.LTGRAY, false));

        Button play = button(L.t("Запустить игру"), v -> startGame());
        play.setTextSize(20); play.setMinHeight(dp(60)); play.setBackgroundTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        play.setEnabled(loc.found && hasStorage()); root.addView(play);
        root.addView(button(L.t("Быстрая настройка"), v -> tour.show()));
        tour.add(play, L.t("Добро пожаловать"), L.t("Краткий обзор лаунчера. Игра запускается этой кнопкой, когда файлы найдены и доступ разрешён."));
        tour.add(lang, L.t("Язык интерфейса"), L.t("Выберите язык лаунчера; язык игры можно изменить отдельно."));
        TextView filesHead = section(L.t("Файлы игры")); root.addView(filesHead);
        tour.add(filesHead, L.t("Файлы игры"), L.t("Проверьте путь к игре и разрешение доступа; здесь также можно выбрать папку."));
        root.addView(text(L.t("Папка игры") + (loc.manual ? L.t(" (выбрана вручную)") : "") + ":\n" + pretty(gameDir), 13, Color.LTGRAY, false));
        statusView = text("", 13, Color.WHITE, false); root.addView(statusView);
        if (!hasStorage()) {
            root.addView(text(L.t("Нет разрешения на доступ к памяти — без него папка /sdcard/MassEffect не видна."), 13, Color.rgb(0xF0, 0xB0, 0x40), false));
            root.addView(button(L.t("Разрешить доступ к файлам"), v -> {
                if (shouldShowRequestPermissionRationale(READ) || !prefs.getBoolean("asked", false)) {
                    prefs.edit().putBoolean("asked", true).apply(); requestPermissions(new String[] { READ, WRITE }, REQ_STORAGE);
                } else startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName())));
            }));
        } else if (Build.VERSION.SDK_INT >= 30 && sdRoot.listFiles() == null) {   /* Android 11+: scoped storage */
            root.addView(text(L.t("Android 11+ не даёт читать /sdcard — включите «Доступ ко всем файлам»."), 13, Color.rgb(0xF0, 0xB0, 0x40), false));
            root.addView(button(L.t("Открыть настройки доступа"), v -> startActivity(new Intent("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION",
                    Uri.parse("package:" + getPackageName())))));
        }
        if (!loc.found)
            root.addView(text(L.t("Скопируйте папку игры (Binaries, BioGame, …) в /sdcard/MassEffect — она найдётся автоматически. ")
                    + L.t("Также ищутся папки «Mass Effect» и похожие в корне памяти и SD-карты (и на уровень глубже)."), 13, Color.LTGRAY, false));
        if (loc.legacyPrivate) {
            root.addView(text(L.t("Игра лежит в папке приложения — она удалится вместе с приложением. Лучше перенести в /sdcard/MassEffect."),
                    13, Color.rgb(0xF0, 0xB0, 0x40), false));
            root.addView(button(L.t("Перенести в /sdcard/MassEffect"), v -> {
                String err = GameLocator.moveGame(gameDir, new File(sdRoot, GameLocator.DEFAULT_NAME));
                if (err != null) new AlertDialog.Builder(this).setTitle(L.t("Перенос")).setMessage(err).setPositiveButton("OK", null).show();
                else toast(L.t("Игра перенесена в /sdcard/MassEffect"));
                relocate(true);
            }));
        }
        root.addView(text(L.t("Сохранения и настройки: ") + pretty(new File(docsDir, "BioWare/Mass Effect")), 12, Color.GRAY, false));
        LinearLayout fileRow = new LinearLayout(this); fileRow.setOrientation(LinearLayout.HORIZONTAL);
        fileRow.addView(button(L.t("Проверить"), v -> showCheck()));
        fileRow.addView(button(L.t("Обновить"), v -> { relocate(true); toast(loc.found ? L.t("Игра: ") + pretty(gameDir) : L.t("Игра не найдена")); }));
        fileRow.addView(button(L.t("Изменить папку"), v -> chooseFolder()));
        HorizontalScrollView fhs = new HorizontalScrollView(this); fhs.addView(fileRow); root.addView(fhs);

        /* v0.10.5: PhoneModify generator (writes <game>/PhoneModify/phone_tune.txt, same texts as me1cfg modgen) */
        Button modgen = button(L.t("Генератор PhoneModify — срезать жор (лимиты кадра, эффекты)"), v -> showModGen()); root.addView(modgen);
        tour.add(modgen, "PhoneModify", L.t("Генератор создаёт настройки лимита кадров и эффектов."));

        TextView controlsHead = section(L.t("Управление и моды")); root.addView(controlsHead);
        tour.add(controlsHead, L.t("Управление и моды"), L.t("Настройте физический геймпад и совместимые моды."));
        buildControls(root);

        TextView gpuHead = section(L.t("Графический драйвер")); root.addView(gpuHead);
        tour.add(gpuHead, L.t("Графический драйвер"), L.t("Выберите системный или установленный графический драйвер."));
        gpuBox = new LinearLayout(this); gpuBox.setOrientation(LinearLayout.VERTICAL); root.addView(gpuBox);
        buildGpu();

        TextView presetHead = section(L.t("Пресеты")); root.addView(presetHead);
        tour.add(presetHead, L.t("Пресеты"), L.t("Автопресет выбирает настройки по устройству; изменения нужно сохранить."));
        HorizontalScrollView hs = new HorizontalScrollView(this);
        LinearLayout row = new LinearLayout(this); row.setOrientation(LinearLayout.HORIZONTAL);
        row.addView(button(L.t("Авто (рекомендуется)"), v -> {
            String r = dev.classify(true);
            if (r.startsWith("-1")) { toast(r.substring(r.indexOf('\n') + 1)); return; }
            pull(Native.dump()); refreshAll(); toast(L.t("Автопресет: ") + r.substring(r.indexOf('\n') + 1) + L.t(" — нажмите «Сохранить»"));
        }));
        for (int i = 0; i < presets.length(); i++) {
            JSONObject p = presets.optJSONObject(i); final String id = p.optString("id");
            Button bt = button(p.optString("label"), v -> {
                String err = L.tx(Native.preset(id));
                if (!err.isEmpty()) { toast(err); return; }
                pull(Native.dump()); refreshAll(); toast(L.t("Пресет применён — нажмите «Сохранить»"));
            });
            row.addView(bt);
        }
        hs.addView(row); root.addView(hs);

        String group = "";
        for (int i = 0; i < settings.length(); i++) {
            JSONObject s = settings.optJSONObject(i);
            if (!s.optString("group").equals(group)) {
                group = s.optString("group"); TextView groupHead = section(group); root.addView(groupHead);
                StringBuilder summary = new StringBuilder();
                for (int j = i; j < settings.length(); j++) { JSONObject item = settings.optJSONObject(j); if (!group.equals(item.optString("group"))) break;
                    String help = item.optString("help"); int end = help.indexOf(". "); if (end >= 0) help = help.substring(0, end + 1);
                    summary.append("• ").append(item.optString("label")).append(" — ").append(help).append("\n"); }
                tour.add(groupHead, group, summary.toString());
                if (s.optString("id").equals("port.gearmass")) { String stats = gmHint(); gmHintView = text(stats, 12, Color.LTGRAY, false); gmHintView.setVisibility(stats.isEmpty() ? View.GONE : View.VISIBLE); root.addView(gmHintView); }
            }
            root.addView(settingRow(s));
        }

        LinearLayout actions = new LinearLayout(this); actions.setOrientation(LinearLayout.HORIZONTAL); actions.setPadding(0, dp(16), 0, 0);
        actions.addView(button(L.t("Сохранить"), v -> {
            String r = L.tx(Native.save());
            boolean ok = r.startsWith("OK");
            new AlertDialog.Builder(this).setTitle(ok ? L.t("Сохранено") : L.t("Ошибка")).setMessage(r.substring(r.indexOf('\n') + 1)).setPositiveButton("OK", null).show();
            reload(); refreshAll();
        }));
        actions.addView(button(L.t("По умолчанию"), v -> { Native.reset(); pull(Native.dump()); refreshAll(); toast(L.t("Значения из Default*.ini — нажмите «Сохранить»")); }));
        root.addView(actions);
        tour.add(actions, L.t("Сохранить и сбросить"), L.t("Сохраните изменения перед запуском; сброс возвращает стандартные значения."));
        root.addView(text(L.t("Игра открывается на весь экран (отдельный процесс). Рендер: Direct3D 9 → OpenGL ES 3. ")
                + L.t("Управление: касание = мышь (палец — левая кнопка, второй палец — правая), «Назад» = Esc, ")
                + L.t("USB/Bluetooth-клавиатура работает как на ПК, геймпад — см. «Управление и моды». Кнопка ⊕ в левом верхнем углу показывает экранное ")
                + L.t("управление: стик (WASD), ЛКМ/ПКМ, E, Пробел, Shift, 1–4 и др.; свайп по свободному месту — камера, ")
                + L.t("долгое нажатие на ⊕ — настройки (прозрачность, размер, чувствительность, перестановка кнопок), ")
                + L.t("короткое касание — щелчок. Ролики (Bink) показываются со звуком, пропуск — «Назад» (Esc) или касание ")
                + L.t("(если ролики мешают — в «Доп. переменных» ME1_BINK=0). Первый запуск компилирует шейдеры ")
                + L.t("(несколько минут; ход компиляции показывается на экране запуска). Лог — Documents/me1exe_run.log, снимки кадров — ")
                + L.t("Documents/me1_frames (первый кадр и каждый 1800-й)."), 12, Color.GRAY, false));
        root.addView(button(L.t("Лог последнего запуска"), v -> showLastLog()));
        Button screenControls = button(L.t("Экранное управление — настройка"), v -> ControlsSettings.show(this, null, null)); root.addView(screenControls);
        tour.add(screenControls, L.t("Экранное управление"), L.t("Три редактируемые раскладки: ПК, геймпад и своя. В игре удерживайте ⊕, чтобы открыть редактор."));
        root.addView(button(L.t("Доп. переменные (для опытов)") + (prefs.getString("extraEnv", "").trim().isEmpty() ? "" : " ✓"), v -> editExtraEnv()));
        Button test = button(L.t("Тест без окна (90 с, кадры → Documents/me1_frames)"), v -> runExeTest());
        test.setEnabled(loc.found && hasStorage()); root.addView(test);
        tour.add(test, L.t("Лог и диагностика"), L.t("Лог помогает искать ошибки; дополнительные переменные и тест без окна предназначены для диагностики."));
        tour.add(play, L.t("Запуск"), L.t("Сохраните настройки и запустите игру. Мастер можно открыть снова кнопкой «Быстрая настройка»."));
        root.addView(text(L.t("Тот же MassEffect.exe, но в отдельной программе без окна: кадры рисуются в память и каждые 300 ")
                + L.t("сохраняются в PNG; завершается через 90 с после первого кадра. Вывод показывается здесь же."), 12, Color.GRAY, false));

        ScrollView sv = new ScrollView(this); sv.setBackgroundColor(Color.rgb(0x10, 0x14, 0x1A)); sv.addView(root);
        tour.scroll = sv;
        return sv;
    }
    private String gmHint() {
        java.util.Map<String,String> m = new java.util.HashMap<>();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(new java.io.FileInputStream(new File(docsDir, "me1_gearmass_last.txt")), "UTF-8"))) {
            for (String line; (line = r.readLine()) != null;) { int eq = line.indexOf('='); if (eq > 0) m.put(line.substring(0,eq), line.substring(eq+1)); }
        } catch (Exception e) { return ""; }
        try {
            String msg = L.t("Последний сеанс GEARMASS") + ": " + m.getOrDefault("fps_avg", "?") + " FPS / " + m.getOrDefault("fps_limit", "?") +
                "\n" + L.t("Режимы (% игрового времени)") + " ECO / BALANCE / PERF / MAX: " + m.getOrDefault("level_eco", "?") + " / " + m.getOrDefault("level_balance", "?") + " / " + m.getOrDefault("level_perf", "?") + " / " + m.getOrDefault("level_max", "?") + "%" +
                "\n" + L.t("Сцены (% сеанса)") + " GPU / CPU / WAIT / VIDEO: " + m.getOrDefault("class_gpu", "?") + " / " + m.getOrDefault("class_cpu", "?") + " / " + m.getOrDefault("class_wait", "?") + " / " + m.getOrDefault("class_video", "?") + "%" +
                "\n" + L.t("Неудачных снижений") + ": " + m.getOrDefault("fails", "?");
            double heat = Double.parseDouble(m.getOrDefault("heat_max", "-1")); if (heat >= 0) msg += " · " + L.t("Нагрев") + ": " + (int) heat + "%";
            if (Double.parseDouble(m.getOrDefault("thermal_partial_seconds", "0")) > 0)
                msg += "\n" + L.t("Данные датчиков были неполными; нагрев мог быть выше.");
            double gpu = Double.parseDouble(m.getOrDefault("class_gpu", "0")), cpu = Double.parseDouble(m.getOrDefault("class_cpu", "0"));
            if (heat >= 90) msg += "\n" + L.t("Высокий нагрев: снизьте настройки или дайте устройству остыть.");
            else if (gpu > cpu && gpu >= 30) msg += "\n" + L.t("Часто ограничивает GPU: попробуйте снизить разрешение или эффекты.");
            else if (cpu >= 30) msg += "\n" + L.t("Часто ограничивает CPU: проверьте фоновые приложения.");
            return msg;
        } catch (Exception e) { return ""; }
    }
    private TextView section(String s) {
        TextView t = text(s, 16, ACCENT, true); t.setPadding(0, dp(18), 0, dp(4)); return t;
    }
    private View settingRow(JSONObject s) {
        final String id = s.optString("id"), type = s.optString("type");
        LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(0, dp(6), 0, dp(6));
        LinearLayout line = new LinearLayout(this); line.setOrientation(LinearLayout.HORIZONTAL); line.setGravity(Gravity.CENTER_VERTICAL);
        TextView label = text(s.optString("label"), 15, Color.WHITE, false);
        line.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(line);
        if (type.equals("bool")) {
            final Switch sw = new Switch(this);
            line.addView(sw);
            refreshers.put(id, () -> sw.setChecked(!"0".equals(values.get(id))));
            sw.setOnCheckedChangeListener((b, on) -> { if (!updating) apply(id, on ? "1" : "0"); });
        } else if (type.equals("enum") || type.equals("resolution")) {
            JSONArray ch = s.optJSONArray("choices");
            final List<String> vals = new ArrayList<>(), labels = new ArrayList<>();
            for (int i = 0; ch != null && i < ch.length(); i++) { vals.add(ch.optJSONArray(i).optString(0)); labels.add(ch.optJSONArray(i).optString(1)); }
            final Spinner sp = new Spinner(this);
            ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
            ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            sp.setAdapter(ad);
            box.addView(sp);
            refreshers.put(id, () -> {
                String v = values.get(id); int idx = vals.indexOf(v);
                if (idx < 0 && v != null) { vals.add(v); labels.add(v.replace('x', '×') + L.t(" (свой)")); ad.notifyDataSetChanged(); idx = vals.size() - 1; }
                if (idx >= 0) sp.setSelection(idx, false);
            });
            sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long l) { if (!updating && !vals.get(pos).equals(values.get(id))) apply(id, vals.get(pos)); }
                @Override public void onNothingSelected(AdapterView<?> p) {}
            });
        } else {   /* int / float with a range */
            final double min = s.optDouble("min"), max = s.optDouble("max");
            final double step = type.equals("float") ? (max - min > 10 ? 0.5 : 0.1) : 1;
            final int steps = (int) Math.round((max - min) / step);
            final TextView val = text("", 15, ACCENT, true); line.addView(val);
            final SeekBar sb = new SeekBar(this); sb.setMax(steps); box.addView(sb);
            refreshers.put(id, () -> {
                String v = values.get(id); double d = 0; try { d = Double.parseDouble(v); } catch (Exception ignored) {}
                sb.setProgress((int) Math.round((d - min) / step)); val.setText(v);
            });
            sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                String fmt(int p) { double d = min + p * step; return type.equals("float") ? String.format(Locale.ROOT, "%.1f", d).replaceAll("\\.0$", "") : String.valueOf((long) d); }
                @Override public void onProgressChanged(SeekBar b, int p, boolean user) { if (user) val.setText(fmt(p)); }
                @Override public void onStartTrackingTouch(SeekBar b) {}
                @Override public void onStopTrackingTouch(SeekBar b) { if (!updating) apply(id, fmt(b.getProgress())); }
            });
        }
        String help = s.optString("help");
        if (!help.isEmpty()) box.addView(text(help, 12, Color.GRAY, false));
        Runnable r = refreshers.get(id); if (r != null) { updating = true; r.run(); updating = false; }
        return box;
    }
    private void apply(String id, String v) {
        String err = L.tx(Native.set(id, v));
        if (!err.isEmpty()) { toast(err); updating = true; refreshers.get(id).run(); updating = false; return; }
        values.put(id, v);
    }
    private void updateStatus() {
        int st = Native.gameStatus(gameDir.getAbsolutePath(), docsDir.getAbsolutePath());
        statusView.setText((st & 1) != 0 && (st & 2) != 0 ? L.t("✓ Игра найдена") + ((st & 4) == 0 ? L.t(" (нет BioGame\\CookedPC)") : "")
                : (st & 1) != 0 ? L.t("✓ Конфиги найдены, ✗ нет Binaries\\MassEffect.exe") : L.t("✗ Игра не найдена — настройки сохранятся и применятся после копирования"));
        statusView.setTextColor((st & 1) != 0 ? Color.rgb(0x8B, 0xD3, 0x6B) : Color.rgb(0xF0, 0xB0, 0x40));
    }
    /** extra NAME=value pairs for the game process; "Список переменных" lists every variable the runtime reads */
    private void editExtraEnv() {
        final EditText et = new EditText(this);
        et.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        et.setText(prefs.getString("extraEnv", "")); et.setMinLines(3);
        TextView info = text(L.t("По одной паре ИМЯ=значение на строку (или через пробел). Пустое поле — всё по умолчанию. ")
                + L.t("Полный список с описаниями — кнопка ниже (касание строки добавляет её в поле)."), 12, Color.LTGRAY, false);
        Button list = button(L.t("Список переменных…"), v -> pickEnv(et));
        LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(et); box.addView(info); box.addView(list);
        new AlertDialog.Builder(this).setTitle(L.t("Доп. переменные")).setView(box)
                .setPositiveButton(L.t("Сохранить"), (d, w) -> { prefs.edit().putString("extraEnv", et.getText().toString()).apply(); relocate(true); })
                .setNeutralButton(L.t("Очистить"), (d, w) -> { prefs.edit().remove("extraEnv").apply(); relocate(true); })
                .setNegativeButton(L.t("Отмена"), null).show();
    }
    /** every environment variable of the runtime: { "NAME=example", description } ("" name: a section header) */
    static final String[][] ENV = {
        { "", "Ролики и звук" },
        { "ME1_BINK=0", "не загружать binkw32.dll: все ролики пропускаются" },
        { "ME1_DSOUND=0", "без DirectSound: ролики без звука" },
        { "ME1_DSOUND_LOG=1", "лог DirectSound (буферы роликов, микшер)" },
        { "ME1_AL=0", "выключить весь звук игры (OpenAL)" },
        { "ME1_AL_GAIN=0.7", "общая громкость (1 = 100 %, можно до 2)" },
        { "ME1_AL_RATE=48000", "частота вывода звука, Гц (8000–192000; по умолчанию — родная частота устройства)" },
        { "ME1_AL_LOG=2", "подробный лог звука (0 — без лога, 1 — по умолчанию)" },
        { "ME1_AL_IMA4=1", "сообщать игре о поддержке сжатия IMA4 ADPCM (по умолчанию нет)" },
        { "ME1_AL_SINK=null", "не выводить звук на устройство (вместе с ME1_AL_WAV — запись микса в файл)" },
        { "ME1_AL_WAV=/sdcard/Documents/me1_audio.wav", "записывать микс в WAV (только с ME1_AL_SINK=null)" },
        { "ME1_AL_TICK_US=10000", "периодические мультимедиа-таймеры игры (звук) срабатывают не чаще раза в N мкс — меньше нагрузка на CPU; микшер работает как обычно (по умолчанию выкл.)" },
        { "ME1_AUDIO_TICK=5", "пауза N мс в микшере, пока не играет ни один звук — меньше нагрузка на CPU в тишине, начало звука может запаздывать (по умолчанию выкл.)" },
        { "", "Графика" },
        { "ME1_FLOAT_EMU=2", "точная эмуляция float D3D9 (0·∞ = 0) ещё и в mul/mad/dp: лечит чёрные/белые точки, медленнее; 0 — выкл., 1 — по умолчанию" },
        { "ME1_SHADER_LITE=1", "упрощённый расчёт (то же, что переключатель «Упрощённый расчёт» в «Графике»): без динамических теней и теней персонажей, простые карты освещения, без размытия/глубины резкости/бликов/декалей, билинейная фильтрация, LOD персонажей ниже; FP16 там, где игра допускает half, таймер 5 мс, звук 32 кГц; 0 — выкл." },
        { "ME1_LITE_INI=0", "упрощённый расчёт без настроек движка (только шейдеры, таймер и звук)" },
        { "ME1_LITE_SKIP=DirectionalLightmaps,DetailMode", "упрощённый расчёт: не трогать перечисленные параметры движка (DynamicShadows, LightEnvironmentShadows, DirectionalLightmaps, MotionBlur, DepthOfField, QualityBloom, LensFlares, DynamicDecals, Trilinear, MaxAnisotropy, SkeletalMeshLODBias, DetailMode, EnableDynamicShadows)" },
        { "ME1_CPU_PIN=0", "не распределять потоки по ядрам (по умолчанию игровой поток и поток рендера — на быстрых ядрах, остальные — на медленных)" },
        { "ME1_PHONE_MOD=1", "применять модификаторы из <папка игры>/PhoneModify (то же, что переключатель «Модификаторы PhoneModify»): лимиты вызовов/примитивов на кадр, ключи INI, патчи кода. Создать модификатор: me1cfg modgen balanced|aggressive" },
        { "ME1_SHOW_FPS=1", "счётчик FPS в правом верхнем углу (то же, что «Показывать FPS» в настройках порта)" },
        { "ME1_CURSOR=2", "курсор мыши игры: 1 — по умолчанию, виден, когда игра его показывает (меню); 2 — всегда; 0 — никогда" },
        { "ME1_VK_ASYNC_PIPE=1", "экспериментальное создание Vulkan pipeline в фоне; если проблемы — удалить переменную или поставить 0" },
        { "ME1_FPS_LIMIT=30", "ограничить частоту кадров (10–240): меньше нагрев и расход батареи; 0 — без ограничения (закрепляет лимит во всех режимах GEARMASS)" },
        { "ME1_RENDERER=vulkan", "рендерер: vulkan — свой D3D9→Vulkan, gles — OpenGL ES, zink — OpenGL ES через Mesa Zink (обычно задаёт лаунчер)" },
        { "ME1_VK_VALIDATE=1", "Vulkan: слой проверки VK_LAYER_KHRONOS_validation, если он есть (медленно, для отладки)" },
        { "ME1_VK_STRICT=1", "Vulkan: вести себя как на самом слабом драйвере (без BC-сжатия, без float-фильтрации) — для проверки" },
        { "ME1_VK_BC=0", "Vulkan: не использовать сжатие BC (DXT), распаковывать текстуры на CPU — если на драйвере текстуры битые или чёрные (по умолчанию BC, если GPU умеет)" },
        { "ME1_VK_SPLIT=2048", "Vulkan: отправлять кадр частями по N вызовов отрисовки — обход падения драйвера Mali (DEVICE_LOST на первом тяжёлом кадре). По умолчанию включено (2048); 0 — одним сабмитом, как в v0.10.2" },
        { "ME1_GPU_TIMER=0", "не замерять время GPU на кадр (строка «[d3d] frame … GPU … ms/frame» в логе)" },
        { "ME1_OCCLUSION=1", "запросы видимости (occlusion queries): 2 — по умолчанию на Android: без ожидания GPU (результат с опозданием на кадр); 1 — как в D3D9, с ожиданием GPU (точно, но может подтормаживать; по умолчанию на ПК); 0 — выкл. (всё считается видимым, как до v1.1.3)" },
        { "ME1_D3D_CMD=2", "буфер команд D3D9 (v1.2.1): 2 — отдельный поток рендера параллельно со следующим кадром игры (по умолчанию на 4+ ядрах, кроме GEARMASS eco/off); 1 — запись и воспроизведение в том же потоке (проверка); 0 — прямые вызовы, как в 1.2" },
        { "ME1_D3D_CMD_CHECK=1", "с ME1_D3D_CMD=1/2: сравнивать полное состояние при каждом вызове отрисовки, расхождения — в лог (медленно, для отладки)" },
        { "ME1_D3D_CMD_MB=64", "размер буфера команд ME1_D3D_CMD, МБ (4–256, по умолчанию 32)" },
        { "ME1_D3D_CMD_SPIN=0", "поток рендера ME1_D3D_CMD=2 без команд сразу засыпает (по умолчанию ждёт 50 мкс): меньше нагрузка на CPU, чуть больше задержка" },
        { "ME1_GEARMASS=auto", "GEARMASS — распределение нагрузки по ядрам: auto (по умолчанию) — по замерам во время игры; eco, balance, perf, max — постоянный режим; custom — значения ME1_GM_*; off — как в 1.2 (то же, что «Режим нагрузки» в разделе «Нагрузка»)" },
        { "ME1_GM_HUD=1", "строка GEARMASS под счётчиком FPS: режим, узкое место (CPU/GPU/ожидание), занятые ядра, загрузка GPU, нагрев" },
        { "ME1_GM_WORKERS=2", "рабочие потоки GEARMASS (распаковка текстур, компиляция шейдеров): число 0–8 закрепляет в любом режиме; auto — по режиму" },
        { "ME1_GM_AHEAD=2", "кадров вперёд (0–2): сколько кадров CPU готовит, пока GPU рисует; закрепляет в любом режиме" },
        { "ME1_GM_SPIN=0", "ожидание главных потоков: 0 — сон, 1 — короткий спин, 2 — спин (быстрее, горячее)" },
        { "ME1_GM_SPIN_US=200", "длительность короткого спина перед сном, мкс (по умолчанию 100)" },
        { "ME1_GM_PRIME=1", "главные потоки игры — на самых быстрых ядрах (по частотам ядер), остальное — на прочих" },
        { "ME1_GM_ADPF_MS=8", "Android: цель времени кадра для подсказок частот системе (ADPF), мс; 0 — не подсказывать" },
        { "ME1_GM_UCLAMP=512", "нижняя граница частоты главных потоков (uclamp.min, 0–1024), если ядро системы разрешает" },
        { "ME1_GM_ANISO=4", "анизотропная фильтрация не ниже (1–16); без переменной её поднимает сам GEARMASS, пока GPU свободен" },
        { "ME1_GM_QUALITY=0", "не поднимать анизотропию автоматически" },
        { "ME1_GM_THERMAL=0.8", "порог запаса по нагреву (0.3–1.5, по умолчанию 0.90): дойдя до него, «Авто» снижает нагрузку на шаг" },
        { "ME1_GM_LOG=0", "строка [gearmass] в логе раз в N секунд (по умолчанию 5; 0 — выключить)" },
        { "ME1_GL_PARALLEL_COMPILE=0", "OpenGL ES: не просить драйвер компилировать шейдеры в нескольких потоках (KHR_parallel_shader_compile)" },
        { "ME1_VK_SPV_MT=0", "Vulkan: компилировать вершинный и пиксельный шейдер по очереди, а не параллельно" },
        { "ME1_INVARIANT=0", "выключить invariant gl_Position: чуть быстрее, но возможны полосы/мерцание на освещённых поверхностях (1 — по умолчанию)" },
        { "ME1_TEX_16=0", "DXT-текстуры (распаковка на CPU) в RGBA8: лучше качество, больше памяти; 1 — по умолчанию (DXT1 → RGB5A1); 2 — ещё DXT3/5 → RGBA4; 3 — DXT1 → RGB565 (точнее цвет, но вырезы DXT1 теряют прозрачность)" },
        { "ME1_TEX_BC=1", "аппаратное сжатие текстур (оба рендера): 2 — по умолчанию, DXT1–5 и ATI1/2, если GPU умеет; 1 — ATI1/2 распаковывать на CPU; 0 — все DXT/ATI на CPU (если текстуры битые или чёрные)" },
        { "ME1_TEX_MAXSIZE=1024", "ограничить размер текстур (512, 1024, 2048): меньше памяти и загрузок, ниже чёткость" },
        { "ME1_GL_NOS3TC=1", "не использовать аппаратное сжатие S3TC/DXT, даже если GPU умеет (распаковка на CPU)" },
        { "ME1_GL_NORGTC=1", "не использовать аппаратное сжатие RGTC (ATI1/ATI2)" },
        { "ME1_VRAM_MB=1024", "сколько видеопамяти сообщать игре, МБ (по умолчанию 512)" },
        { "ME1_SCREEN=1280x720", "разрешение «рабочего стола» для игры (по умолчанию — экран устройства)" },
        { "ME1_GL=0", "без рендера (чёрный экран) — только для поиска падений" },
        { "ME1_GPU_DRIVER=vulkan", "графический драйвер: vulkan — свой рендерер на системном Vulkan, zink — Mesa Zink на системном Vulkan, /путь/vulkan.xxx.so — свой драйвер (по умолчанию со своим рендерером, ME1_GPU_ZINK=1 — через Zink), system — системный GLES (обычно задаёт лаунчер, раздел «Графический драйвер»)" },
        { "ME1_GL_PRESENT=cpu", "выводить кадры через CPU (ANativeWindow) и с системным GLES — для проверки; с Zink включается сам" },
        { "ZINK_DEBUG=compact", "отладочные флаги Mesa Zink (compact, nobgc, …); MESA_LOG/MESA_DEBUG тоже работают" },
        { "", "Кэши" },
        { "ME1_GL_CACHE=off", "не сохранять собранные GL-программы (кэш в памяти приложения, ускоряет повторные запуски)" },
        { "ME1_HLSL_CACHE=off", "не использовать кэш скомпилированных HLSL-шейдеров (Documents/me1_hlsl_cache): компиляция при каждом запуске" },
        { "ME1_VFS_CACHE=0", "без кэша каталогов (файловый слой Win32)" },
        { "", "Время, потоки, ввод" },
        { "ME1_MM_PERIOD=1", "период мультимедиа-таймера, мс (1–100, по умолчанию 2)" },
        { "ME1_MM=0", "выключить мультимедиа-таймеры (timeSetEvent)" },
        { "ME1_SPIN_SLEEP_US=0", "пауза в циклах ожидания Sleep(0), мкс (по умолчанию 100; 0 — только yield, больше нагрузка на CPU)" },
        { "ME1_TOUCH_FRAMES=2", "через сколько кадров после касания игра получает нажатие кнопки мыши (курсор успевает переместиться)" },
        { "ME1_TOUCH_HOLD=2", "сколько кадров кнопка мыши остаётся нажатой перед отпусканием" },
        { "ME1_XINPUT=0", "геймпад XInput: 0 — нет, 1 — подключён всегда (ставит лаунчер в профиле «Геймпад»), по умолчанию — после первого нажатия" },
        { "", "Снимки кадров и логи" },
        { "ME1_FRAME_DUMP_EVERY=300", "сохранять каждый N-й кадр в Documents/me1_frames (по умолчанию 1800)" },
        { "ME1_FRAME_DUMP=", "(пустое значение) не сохранять кадры" },
        { "ME1_D3D_LOG=1", "лог вызовов Direct3D (2 — очень подробно и медленно)" },
        { "ME1_GL_DEBUG=1", "проверка ошибок OpenGL после каждого вызова (медленно)" },
        { "ME1_GLSL_DUMP=/sdcard/Documents/me1_glsl", "сохранять исходники GLSL программ, которые не собрались" },
        { "ME1_D3D_DUMP=/sdcard/Documents/me1_shaders", "сохранять байткод всех шейдеров (shaders.pak)" },
        { "ME1_HB=5", "период строки состояния [hb] в логе, с (0 — выкл., по умолчанию 10)" },
        { "ME1_PROF_HZ=200", "частота сэмплирующего профилировщика в строке [hb], Гц (по умолчанию 100)" },
        { "ME1_ECHO_LOG=0", "не копировать лог движка (Logs/*.log) в me1exe_run.log" },
        { "ME1_VFS_TRACE=1", "лог обращений к файлам (2 — подробнее)" },
        { "ME1_TRACE_INPUT=1", "лог ввода (мышь, клавиатура)" },
        { "ME1_TRACE_UI=1", "лог оконных сообщений и курсора" },
        { "ME1_TRACE_THREADS=1", "лог создания потоков и синхронизации" },
        { "ME1_TRACE_EH=1", "лог исключений (SEH / C++)" },
        { "ME1_TRACE_REG=1", "лог обращений к реестру" },
        { "ME1_LIST_MISSING=1", "перечислить все нереализованные функции Windows" },
        { "ME1_HEAP_NOREUSE=1", "не переиспользовать освобождённую память (поиск ошибок; память растёт!)" },
        { "ME1_WATCH=1", "включить сторож памяти PhysX (следит за 12283ce0; по умолчанию выключен с v1.1)" },
        { "ME1_MAX_SECONDS=120", "закрыть игру через N секунд после первого кадра" },
        { "ME1_MAX_FRAMES=5000", "закрыть игру после N кадров" },
        { "", "Задаёт лаунчер (можно переопределить)" },
        { "ME1_D3DX=hle", "не загружать d3dx9_31.dll — без компилятора HLSL шейдеры не соберутся (all — все функции D3DX из dll)" },
        { "ME1_D3DX_DLL=/путь/d3dx9_31.dll", "свой d3dx9_31.dll (по умолчанию — из APK)" },
        { "ME1_MODS=/sdcard/MassEffect/Mods/ME1Controller", "папки модов через «:» (поверх игры и Documents/…/Config; лаунчер ставит включённые моды)" },
    };
    private void pickEnv(final EditText et) {
        final String[] items = new String[ENV.length];
        for (int i = 0; i < ENV.length; i++) items[i] = ENV[i][0].isEmpty() ? "— " + L.t(ENV[i][1]) + " —" : L.t(ENV[i][0]) + "\n    " + L.t(ENV[i][1]);
        new AlertDialog.Builder(this).setTitle(L.t("Переменные (касание — добавить)")).setItems(items, (d, i) -> {
            String kv = L.t(ENV[i][0]); if (kv.isEmpty()) return;
            String name = kv.substring(0, kv.indexOf('=') + 1);
            StringBuilder b = new StringBuilder();
            for (String l : et.getText().toString().split("\\s+")) if (!l.isEmpty() && !l.startsWith(name)) b.append(l).append('\n');
            b.append(kv).append('\n');
            et.setText(b.toString()); et.setSelection(et.getText().length());
            toast(kv + L.t(" — поправьте значение при необходимости"));
        }).setNegativeButton(L.t("Закрыть"), null).show();
    }
    /** v0.10.5: PhoneModify generator — preset chooser; texts mirror me1cfg modgen (settings/me1cfg_cli.c) */
    private void showModGen() {
        String soc = "";   /* v0.10.13: Build.SOC_MODEL is API 31 - not in the android-29 jar, read it by reflection */
        if (Build.VERSION.SDK_INT >= 31) try { Object v = Build.class.getField("SOC_MODEL").get(null); if (v != null) soc = v.toString(); } catch (Throwable ignored) { }
        String hw = (Build.HARDWARE + " " + soc + " " + Build.MODEL).toLowerCase(Locale.US);
        String auto = "balanced";
        if (hw.contains("mt67") || hw.contains("helio") || hw.contains("ums") || hw.contains("sm61") || hw.contains("sm62") || hw.contains("sdm6")) auto = "aggressive";
        if (hw.contains("msm89") || hw.contains("sdm4") || hw.contains("sm42") || hw.contains("sm43") || hw.contains("sc9863") || hw.contains("ums9230")) auto = "extreme";
        final String autoLevel = auto;
        String[] items = {
            L.t("Авто — по устройству (SoC: ") + hw.trim() + " → " + autoLevel + L.t(")  [рекомендуется]"),
            L.t("light — только тени (почти без потерь)"),
            L.t("balanced — частицы/тени/декали/LOD, лимит 6000 вызовов и 3 млн примитивов"),
            L.t("aggressive — + MotionBlur/DoF/блум выкл, сцена 70%, лимит 3500 и 1,8 млн"),
            L.t("extreme — всё по минимуму, сцена 60%, лимит 2500 и 1,2 млн"),
            L.t("cpu — для мощных GPU с медленным CPU: без снижения разрешения, лимит 1200 и 300 тыс."),
            L.t("off — убрать модификатор и выключить режим") };
        new AlertDialog.Builder(this).setTitle(L.t("Генератор PhoneModify"))
            .setItems(items, (d, w) -> {
                String lvl = w == 0 ? autoLevel : new String[] {"", "light", "balanced", "aggressive", "extreme", "cpu", "off"}[w];
                writeModGen(lvl);
            }).setNegativeButton(L.t("Отмена"), null).show();
    }
    private String modGenText(String lvl) {
        StringBuilder b = new StringBuilder();
        b.append("# PhoneModify: ").append(lvl).append("\n")
         .append(L.t("# Включить: переключатель \"Модификаторы PhoneModify\" в лаунчере (или ME1_PHONE_MOD=1).\n"))
         .append(L.t("# Проверка в логе: [mod] PhoneModify active: ... и ', capped N draws' в строке кадра.\n\n"));
        if (lvl.equals("off")) return b.toString();
        if (!lvl.equals("light")) b.append("ini SystemSettings ParticleLODBias ").append(lvl.equals("balanced") ? "1" : "2").append(L.t("          # частицы беднее: снегопад PeakDrift и прочий жор\n"));
        b.append(L.t("ini SystemSettings DynamicShadows False\nini SystemSettings LightEnvironmentShadows False    # проходы теней\n"));
        if (!lvl.equals("light")) b.append(L.t("ini SystemSettings DynamicDecals False        # кровь/следы: каждая перерисовывает геометрию\n"));
        if (lvl.equals("aggressive") || lvl.equals("extreme"))
            b.append("ini SystemSettings MotionBlur False\nini SystemSettings DepthOfField False\nini SystemSettings LensFlares False\nini SystemSettings QualityBloom False\n");
        if (lvl.equals("extreme")) b.append(L.t("ini SystemSettings DirectionalLightmaps False   # 1 карта освещения вместо 3\n"));
        if (lvl.equals("aggressive") || lvl.equals("extreme")) b.append(L.t("ini SystemSettings DetailMode 1              # мелкие детали окружения\n"));
        if (!lvl.equals("light")) b.append("ini SystemSettings SkeletalMeshLODBias ").append(lvl.equals("extreme") ? "2" : "1").append(L.t("            # персонажи на LOD ниже\n"));
        if (lvl.equals("cpu")) { b.append("maxdraws 1200\nmaxprims 300000\n"); }
        if (lvl.equals("extreme")) b.append(L.t("ini SystemSettings MaxAnisotropy 1                 # анизотропия\n"));
        if (lvl.equals("aggressive")) b.append(L.t("ini SystemSettings ScreenPercentage 70            # сцена в 70% разрешения, интерфейс полный\n"));
        if (lvl.equals("extreme")) b.append(L.t("ini SystemSettings ScreenPercentage 60            # сцена в 60% разрешения, интерфейс полный\n"));
        if (lvl.equals("balanced")) { b.append("maxdraws 6000\nmaxprims 3000000\n"); }
        if (lvl.equals("aggressive")) { b.append("maxdraws 3500\nmaxprims 1800000\n"); }
        if (lvl.equals("extreme")) { b.append("maxdraws 2500\nmaxprims 1200000\n"); }
        b.append(L.t("\n# Тонкая настройка вручную: раскомментируй и поправь.\n# maxdraws 2000\n# maxprims 800000\n# ini SystemSettings ScreenPercentage 50\n# patch 10e7dc00 90 90 90        # пример: байты по адресу из лога (m_10e7dc00)\n"));
        return b.toString();
    }
    private void writeModGen(String lvl) {
        try {
            File dir = new File(gameDir, "PhoneModify");
            File f = new File(dir, "phone_tune.txt");
            if (lvl.equals("off")) {
                if (f.exists()) f.delete();
                Native.set("port.phone_mod", "0"); Native.save();
                toast(L.t("phone_tune.txt удалён, модификаторы выключены"));
                return;
            }
            dir.mkdirs();
            FileWriter o = new FileWriter(f);
            o.write(modGenText(lvl)); o.close();
            Native.set("port.phone_mod", "1"); Native.save();
            toast(L.t("Создан PhoneModify/phone_tune.txt (") + lvl + L.t("), модификаторы включены"));
        } catch (Throwable t) {
            new AlertDialog.Builder(this).setTitle(L.t("Ошибка")).setMessage(String.valueOf(t)).setPositiveButton("OK", null).show();
        }
    }
    private void chooseFolder() {
        final EditText et = new EditText(this);
        et.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        et.setText(pretty(gameDir)); et.setSelectAllOnFocus(true);
        StringBuilder sb = new StringBuilder(L.t("Путь к папке с Binaries и BioGame. «Авто» — искать /sdcard/MassEffect и похожие.\nПроверено:"));
        for (File f : loc.searched) sb.append("\n").append(GameLocator.isGame(f) ? "✓ " : "· ").append(pretty(f));
        TextView info = text(sb.toString(), 12, Color.LTGRAY, false);
        LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(et); box.addView(info);
        new AlertDialog.Builder(this).setTitle(L.t("Папка игры")).setView(box)
                .setPositiveButton(L.t("Выбрать"), (d, w) -> {
                    String p = et.getText().toString().trim();
                    if (p.startsWith("/sdcard")) p = sdRoot.getAbsolutePath() + p.substring(7);
                    if (!GameLocator.isGame(new File(p)) && GameLocator.findGame(new File(p)) == null) toast(L.t("В этой папке игра не найдена — путь всё равно сохранён"));
                    prefs.edit().putString("gameDir", p).apply(); relocate(true);
                })
                .setNeutralButton(L.t("Авто"), (d, w) -> { prefs.edit().remove("gameDir").apply(); relocate(true); })
                .setNegativeButton(L.t("Отмена"), null).show();
    }
    private void showCheck() {
        updateStatus();
        TextView t = text(L.tx(Native.check(gameDir.getAbsolutePath(), docsDir.getAbsolutePath())), 12, Color.WHITE, false);
        t.setTypeface(Typeface.MONOSPACE); t.setPadding(dp(16), dp(8), dp(16), dp(8)); t.setTextIsSelectable(true);
        ScrollView sv = new ScrollView(this); sv.addView(t);
        new AlertDialog.Builder(this).setTitle(L.t("Что увидит игра")).setView(sv).setPositiveButton("OK", null).show();
    }
    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_LONG).show(); }

    /** Runs the recompiled MassEffect.exe (static arm64 executable packaged as lib/arm64-v8a/libme1exe.so).
     * Android 10+ forbids executing files from app data or /sdcard (W^X, noexec) - only nativeLibraryDir is executable. */
    private void runExeTest() {
        final File exe = new File(getApplicationInfo().nativeLibraryDir, "libme1run.so");
        if (!exe.exists()) { toast(L.t("В этой сборке APK нет libme1run.so")); return; }
        prepareDriver(true, drv -> runExeTest(exe, drv));
    }
    private void runExeTest(final File exe, final Map<String, String> drv) {
        final Map<String, String> envAll = gameEnv(true, drv);
        final TextView out = text(L.t("Запуск ") + exe + "\n", 11, Color.WHITE, false);
        out.setTypeface(Typeface.MONOSPACE); out.setTextIsSelectable(true); out.setPadding(dp(12), dp(8), dp(12), dp(8));
        final ScrollView sv = new ScrollView(this); sv.addView(out);
        new AlertDialog.Builder(this).setTitle(L.t("Тест без окна: MassEffect.exe")).setView(sv).setPositiveButton(L.t("Закрыть"), null).show();
        final File game = gameDir, docs = docsDir, log = new File(docsDir, "me1exe_run.log");
        new Thread(() -> {
            String res;
            long t0 = System.currentTimeMillis();
            try {
                docs.mkdirs();
                ProcessBuilder pb = new ProcessBuilder(exe.getAbsolutePath(), game.getAbsolutePath(), docs.getAbsolutePath(), "-");
                pb.redirectErrorStream(true); pb.directory(getFilesDir());
                pb.environment().putAll(envAll);
                final Process p = pb.start();
                Thread dog = new Thread(() -> { try { Thread.sleep(1800000); p.destroy(); } catch (InterruptedException e) { } });
                dog.setDaemon(true); dog.start();
                java.io.Writer w = null;
                try { w = new java.io.FileWriter(log); } catch (Exception e) { post(out, L.t("(лог не записан: ") + e.getMessage() + ")"); }
                try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream(), "UTF-8"))) {
                    String line;
                    while ((line = r.readLine()) != null) {
                        if (w != null) { w.write(line); w.write('\n'); }
                        if (interesting(line))
                            post(out, line.length() > 400 ? line.substring(0, 400) + " …" : line);
                    }
                }
                if (w != null) w.close();
                dog.interrupt();
                res = L.t("\nКод завершения: ") + p.waitFor() + ", " + (System.currentTimeMillis() - t0) / 1000.0 + L.t(" с");
            } catch (Exception e) { res = L.t("\nОшибка запуска: ") + e; }
            post(out, res + "\n\n" + engineLog(new File(docs, "BioWare/Mass Effect/Logs")));
        }).start();
    }
    /** environment of a game run (GameActivity or the test executable) */
    private Map<String, String> gameEnv(boolean test, Map<String, String> drv) {
        Map<String, String> env = new java.util.LinkedHashMap<>();
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        int sw = Math.max(dm.widthPixels, dm.heightPixels), sh = Math.min(dm.widthPixels, dm.heightPixels);
        env.put("ME1_SCREEN", sw + "x" + sh);
        File dx = d3dxDll();
        if (dx != null) env.put("ME1_D3DX_DLL", dx.getAbsolutePath());
        File frames = new File(docsDir, "me1_frames"); frames.mkdirs();
        File[] old = frames.listFiles();
        if (old != null) for (File f : old) if (f.getName().endsWith(".png")) f.delete();
        env.put("ME1_FRAME_DUMP", frames.getAbsolutePath());
        env.put("ME1_FRAME_DUMP_EVERY", test ? "300" : "1800");
        File glc = new File(getFilesDir(), "gl_cache"); glc.mkdirs();
        env.put("ME1_GL_CACHE", glc.getAbsolutePath());
        env.putAll(drv);   /* ME1_GPU_DRIVER, own GL cache per driver */
        StringBuilder mods = new StringBuilder();
        for (File m : enabledMods()) mods.append(mods.length() > 0 ? ":" : "").append(m.getAbsolutePath());
        if (mods.length() > 0) env.put("ME1_MODS", mods.toString());
        if (ControlsSettings.prefs(this).getBoolean("xinput", false)) env.put("ME1_XINPUT", "1");
        /* "Упрощённый расчёт" (me1port.ini, port.simple_shaders) -> shaders, timers, audio mixing; FPS counter and limit ([Port] ShowFps/FpsLimit) */
        try {
            String dd = docsDir.getAbsolutePath();
            if ("1".equals(Native.portOption(dd, "port.simple_shaders"))) env.put("ME1_SHADER_LITE", "1");
            if ("1".equals(Native.portOption(dd, "port.phone_mod"))) env.put("ME1_PHONE_MOD", "1");   /* v0.10.4: PhoneModify */
            if ("1".equals(Native.portOption(dd, "port.show_fps"))) env.put("ME1_SHOW_FPS", "1");
            String fl = Native.portOption(dd, "port.fps_limit");
            if (fl != null && fl.matches("\\d+") && Integer.parseInt(fl) > 0) env.put("ME1_FPS_LIMIT", fl);
            /* v1.2.1 GEARMASS: section «Нагрузка» (me1port.ini [Port] Gearmass*); "Доп. переменные" below win */
            String gm = Native.portOption(dd, "port.gearmass");
            if (gm != null && !gm.isEmpty()) env.put("ME1_GEARMASS", gm);
            String sm = Native.portOption(dd, "port.stretch_mode");
            if (sm != null && !sm.isEmpty()) env.put("ME1_STRETCH_MODE", sm);
            if ("1".equals(Native.portOption(dd, "port.gm_hud"))) env.put("ME1_GM_HUD", "1");
            String th = Native.portOption(dd, "port.gm_thermal");
            if (th != null && th.matches("\\d+") && Integer.parseInt(th) > 0 && Integer.parseInt(th) != 90)
                env.put("ME1_GM_THERMAL", String.format(Locale.ROOT, "%.2f", Integer.parseInt(th) / 100.0));
            if ("custom".equals(gm)) {
                String[][] gk = { { "port.gm_workers", "ME1_GM_WORKERS" }, { "port.gm_ahead", "ME1_GM_AHEAD" }, { "port.gm_spin", "ME1_GM_SPIN" },
                        { "port.gm_prime", "ME1_GM_PRIME" }, { "port.gm_aniso", "ME1_GM_ANISO" } };
                for (String[] k : gk) { String v = Native.portOption(dd, k[0]); if (v != null && !v.isEmpty() && !"auto".equals(v)) env.put(k[1], v); }
            }
        } catch (Throwable ignored) {}
        for (String kv : prefs.getString("extraEnv", "").trim().split("\\s+")) {
            int eq = kv.indexOf('=');
            if (eq > 0) env.put(kv.substring(0, eq), kv.substring(eq + 1));
        }
        if (test) {
            env.put("ME1_GM_RES_SKIP", "1");
            env.put("ME1_D3D_DUMP", new File(docsDir, "me1_shaders").getAbsolutePath());
            env.put("ME1_MAX_SECONDS", "90");
            env.put("ME1_MAX_FRAMES", "20000");
        }
        return env;
    }
    private void startGame() {
        String pm = syncPadMod();   /* v1.1.4: a controller mod with the keyboard/mouse profile - warn before starting */
        if (pm != null && !ControlsSettings.prefs(this).getBoolean("xinput", false)) { padModWarning(pm, () -> prepareDriver(false, this::startGame)); return; }
        prepareDriver(false, this::startGame);
    }
    private void startGame(Map<String, String> drv) {
        docsDir.mkdirs();
        Map<String, String> env = gameEnv(false, drv);
        String[] ev = new String[env.size()]; int i = 0;
        for (Map.Entry<String, String> e : env.entrySet()) ev[i++] = e.getKey() + "=" + e.getValue();
        Intent it = new Intent(this, GameActivity.class);
        it.putExtra("args", new String[] { gameDir.getAbsolutePath(), docsDir.getAbsolutePath(), "-" });
        it.putExtra("env", ev);
        it.putExtra("log", new File(docsDir, "me1exe_run.log").getAbsolutePath());
        gamePending = true; startActivity(it);
    }
    private static boolean interesting(String line) {
        return line.startsWith("[rt]") || line.startsWith("[exe]") || line.startsWith("[d3d") || line.startsWith("[gl]") || line.startsWith("[vk]") || line.startsWith("[bink]")
                || line.startsWith("[hb]") || line.startsWith("[log]") || line.startsWith("[dinput]") || line.startsWith("[ui]")
                || line.startsWith("[al]") || line.startsWith("[mm]") || line.startsWith("[hle] MessageBox") || line.contains("distinct APIs called")
                || line.contains("FAULT") || line.contains("CRASH") || line.startsWith("  ") || line.startsWith("[hle] unimplemented") || line.startsWith("[hle] missing");
    }
    private void showLastLog() {
        File log = new File(docsDir, "me1exe_run.log");
        java.util.ArrayDeque<String> tail = new java.util.ArrayDeque<>();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(new java.io.FileInputStream(log), "UTF-8"))) {
            String l;
            while ((l = r.readLine()) != null) if (interesting(l)) { tail.add(l.length() > 300 ? l.substring(0, 300) + " …" : l); if (tail.size() > 400) tail.poll(); }
        } catch (Exception e) { tail.add(L.t("Нет лога: ") + e.getMessage()); }
        StringBuilder sb = new StringBuilder(); for (String l : tail) sb.append(l).append('\n');
        TextView t = text(sb.toString(), 10, Color.WHITE, false); t.setTypeface(Typeface.MONOSPACE); t.setTextIsSelectable(true);
        t.setPadding(dp(12), dp(8), dp(12), dp(8));
        ScrollView sv = new ScrollView(this); sv.addView(t);
        new AlertDialog.Builder(this).setTitle(log.getName()).setView(sv).setPositiveButton("OK", null).show();
        sv.post(() -> sv.fullScroll(View.FOCUS_DOWN));
    }
    /** d3dx9_31.dll (HLSL compiler, recompiled into libme1exe.so) ships in the APK assets: its code runs from the
     * executable, the original file is only mapped as the module's data. Copied to app storage on first use. */
    private File d3dxDll() {
        File f = new File(getFilesDir(), "d3dx9_31.dll");
        try (java.io.InputStream in = getAssets().open("d3dx9_31.dll")) {
            byte[] b = new byte[1 << 16]; java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            int n; while ((n = in.read(b)) > 0) bo.write(b, 0, n);
            byte[] data = bo.toByteArray();
            if (!f.exists() || f.length() != data.length) {
                File tmp = new File(getFilesDir(), "d3dx9_31.dll.tmp");
                try (java.io.FileOutputStream o = new java.io.FileOutputStream(tmp)) { o.write(data); }
                if (!tmp.renameTo(f)) return null;
            }
            return f;
        } catch (Exception e) { return null; }
    }
    private void post(final TextView t, final String line) { runOnUiThread(() -> t.append(line + "\n")); }
    private static String engineLog(File dir) {
        File[] fs = dir.listFiles(); File last = null;
        if (fs != null) for (File f : fs) if (f.getName().endsWith(".log") && (last == null || f.lastModified() > last.lastModified())) last = f;
        if (last == null) return L.t("Лог движка не найден в ") + dir;
        StringBuilder sb = new StringBuilder(L.t("Лог движка (") + last.getName() + "):\n");
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(new java.io.FileInputStream(last), "UTF-8"))) {
            String l; int n = 0;
            while ((l = r.readLine()) != null && n++ < 60) sb.append(l.replace("\u0000", "")).append('\n');
        } catch (Exception e) { sb.append(e); }
        return sb.toString();
    }
}
