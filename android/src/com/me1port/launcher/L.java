package com.me1port.launcher;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Locale;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * v1.0: launcher UI languages = the game's text languages. The source strings are Russian (they are the keys);
 * translations live in assets/i18n/<code>.json as { "русский текст": "translation" }.
 * Lookup: current language → English → the Russian original. Preference "ui_lang" in SharedPreferences "me1port"
 * (read by every process: the launcher and the game process both call init()).
 */
final class L {
    static final String[] CODES = { "ru", "en", "de", "fr", "it", "es", "pl", "cs", "hu" };
    static final String[] NAMES = { "Русский", "English", "Deutsch", "Français", "Italiano", "Español", "Polski", "Čeština", "Magyar" };
    /** matching value of the game setting game.language (Language in [Engine.Engine]) */
    static final String[] GAME = { "RU", "INT", "DE", "FR", "IT", "ES", "PL", "CZ", "HU" };
    /** pieces of native (C) messages: autopreset report, save / check reports, set() errors; replaced longest first */
    private static final String[] FRAGS = {
        "GPU не распознан (", " → класс «", "; мало ОЗУ → «", "; CPU до ", " ГГц → «", "ОЗУ ", " ГБ, ", " ядер", "до ", " ГГц, ",
        "экран ", "рендер ", "минимальный", "низкий", "средний", "высокий", "максимальный",
        "ошибка: ", "не удалось создать папку для ", "ошибка записи ", "переопределения: ",
        ".ini ещё не создан игрой — значения войдут в него при первом запуске", "обновлён: ", "настройки не загружены",
        "Папка игры: ", "Мои документы (для игры): ", " (ошибка)", "BIOEngine.ini (создан игрой)", "DefaultEngine.ini + настройки порта",
        "Конфигурация игры не найдена — скопируйте игру в папку выше.", "Игра прочитает (",
        "Разрешение X", "Разрешение Y", "Детализация текстур", "Фильтрация", "Динамические тени", "Постобработка", "Каналов звука", "Гамма", "Язык",
        ": ожидается 0/1", ": ожидается целое", ": допустимо ", ": ожидается число", ": формат ШИРИНАxВЫСОТА, от 320x200",
        ": недопустимое значение '", "неизвестная настройка '", "неизвестный пресет '" };
    static { Arrays.sort(FRAGS, new Comparator<String>() { @Override public int compare(String a, String b) { return b.length() - a.length(); } }); }

    private static int cur = -1;
    private static Map<String, String> tr = new HashMap<>(), en = new HashMap<>();
    private L() {}

    static SharedPreferences prefs(Context c) { return c.getSharedPreferences("me1port", Context.MODE_MULTI_PROCESS); }

    /** language index: saved choice, else the system language when it is one of ours, else English */
    static int saved(Context c) {
        String code = prefs(c).getString("ui_lang", null);
        if (code == null) code = Locale.getDefault().getLanguage();
        if ("cz".equals(code)) code = "cs";
        for (int i = 0; i < CODES.length; i++) if (CODES[i].equals(code)) return i;
        return 1;
    }
    static void init(Context c) {
        int i = saved(c);
        if (i == cur) return;
        cur = i;
        en = cur == 0 ? new HashMap<String, String>() : load(c, "en");
        tr = cur <= 1 ? en : load(c, CODES[cur]);
    }
    static void set(Context c, int i) {
        prefs(c).edit().putString("ui_lang", CODES[i]).commit();
        cur = -1; init(c);
    }
    static int cur() { return cur < 0 ? 0 : cur; }

    private static Map<String, String> load(Context c, String code) {
        Map<String, String> m = new HashMap<>();
        try (InputStream in = c.getAssets().open("i18n/" + code + ".json")) {
            ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] b = new byte[16384]; int n;
            while ((n = in.read(b)) > 0) bo.write(b, 0, n);
            JSONObject o = new JSONObject(new String(bo.toByteArray(), "UTF-8"));
            for (Iterator<String> it = o.keys(); it.hasNext(); ) { String k = it.next(); m.put(k, o.getString(k)); }
        } catch (Exception ignored) {}
        return m;
    }

    /** translate one source (Russian) string */
    static String t(String s) {
        if (s == null || cur <= 0) return s;
        String r = tr.get(s);
        if (r == null) r = en.get(s);
        return r != null ? r : s;
    }
    static String[] arr(String[] a) { String[] r = new String[a.length]; for (int i = 0; i < a.length; i++) r[i] = t(a[i]); return r; }

    /** translate the pieces of a native message */
    static String tx(String s) {
        if (s == null || cur <= 0) return s;
        for (String f : FRAGS) {
            if (!s.contains(f)) continue;
            String r = t(f);
            StringBuilder b = new StringBuilder(); int from = 0, j;
            while ((j = s.indexOf(f, from)) >= 0) {
                b.append(s, from, j).append(r); from = j + f.length();
                int sp = 0; while (from + sp < s.length() && s.charAt(from + sp) == ' ') sp++;
                if (sp >= 2) {   /* a padded column (the "what the game will see" table): keep the values aligned */
                    for (int k = Math.max(1, f.length() + sp - r.length()); k > 0; k--) b.append(' ');
                    from += sp;
                }
            }
            s = b.append(s.substring(from)).toString();
        }
        return s;
    }

    /** settings schema from Native.schema(): groups, labels, help texts, choice labels, presets */
    static void schema(JSONObject s) throws Exception {
        if (cur <= 0) return;
        JSONArray st = s.getJSONArray("settings");
        for (int i = 0; i < st.length(); i++) {
            JSONObject o = st.getJSONObject(i);
            for (String k : new String[] { "group", "label", "help" }) if (o.has(k)) o.put(k, t(o.getString(k)));
            JSONArray ch = o.optJSONArray("choices");
            for (int j = 0; ch != null && j < ch.length(); j++) { JSONArray p = ch.getJSONArray(j); p.put(1, t(p.getString(1))); }
        }
        JSONArray pr = s.getJSONArray("presets");
        for (int i = 0; i < pr.length(); i++) {
            JSONObject o = pr.getJSONObject(i);
            for (String k : new String[] { "label", "help" }) if (o.has(k)) o.put(k, t(o.getString(k)));
        }
    }
}
