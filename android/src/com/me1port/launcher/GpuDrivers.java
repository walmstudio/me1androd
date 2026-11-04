package com.me1port.launcher;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Custom Vulkan drivers (v0.9.8) in <game>/GameExDriver: AdrenoTools packages (.zip or folder with meta.json + .so files,
 * as used by Skyline/yuzu/Eden/Winlator: Turnip, Qualcomm and Mali builds) or a bare vulkan .so. The selected package is
 * unpacked flat into app storage (Android loads libraries only from there, not from /sdcard) and handed to the game as
 * ME1_GPU_DRIVER=<dir>/<libraryName>; the runtime opens it with libadrenotools and draws through Mesa zink. */
final class GpuDrivers {
    static final String FOLDER = "GameExDriver";

    static final class Driver {
        File src;                       /* zip, folder or .so inside GameExDriver */
        String id = "", name = "", description = "", author = "", packageVersion = "", vendor = "", driverVersion = "", libraryName = "";
        int minApi;
        String error;                   /* not usable: why */
        String label() {
            return name + (!driverVersion.isEmpty() && !name.contains(driverVersion) ? " " + driverVersion : "")
                    + (!vendor.isEmpty() && !name.toLowerCase(Locale.ROOT).contains(vendor.toLowerCase(Locale.ROOT)) ? " (" + vendor + ")" : "");
        }
        /** "adreno" / "mali" / "" from the package texts */
        String family() { return GpuDrivers.family(vendor + " " + name + " " + description + " " + libraryName); }
        String details() {
            StringBuilder b = new StringBuilder();
            b.append(L.t("Файл: ")).append(src.getName());
            if (!libraryName.isEmpty()) b.append(L.t(" · библиотека ")).append(libraryName);
            if (!packageVersion.isEmpty()) b.append(L.t(" · версия пакета ")).append(packageVersion);
            if (!author.isEmpty()) b.append(L.t(" · автор: ")).append(author);
            if (minApi > 0) b.append(" · Android API ≥ ").append(minApi);
            if (!description.isEmpty()) b.append('\n').append(description);
            return b.toString();
        }
    }

    static String family(String text) {
        String t = text.toLowerCase(Locale.ROOT);
        if (t.contains("adreno") || t.contains("qualcomm") || t.contains("turnip") || t.contains("freedreno") || t.contains("snapdragon")
                || t.contains("vulkan.adr") || t.contains("qcom")) return "adreno";
        if (t.contains("mali") || t.contains("panfrost") || t.contains("panvk") || t.contains("malisx")) return "mali";
        if (t.contains("powervr") || t.contains("imagination") || t.contains("pvr")) return "powervr";
        if (t.contains("xclipse") || t.contains("radv") || t.contains("samsung")) return "xclipse";
        return "";
    }
    static String familyName(String f) {
        return f.equals("adreno") ? "Adreno (Qualcomm)" : f.equals("mali") ? "Mali (ARM)" : f.equals("powervr") ? "PowerVR" : f.equals("xclipse") ? "Xclipse (Samsung)" : "?";
    }

    /** <game>/GameExDriver (any letter case); created if missing so the user sees where to put drivers */
    static File folder(File gameDir) {
        File[] fs = gameDir.listFiles();
        if (fs != null) for (File f : fs) if (f.isDirectory() && f.getName().equalsIgnoreCase(FOLDER)) return f;
        File f = new File(gameDir, FOLDER);
        if (gameDir.isDirectory()) f.mkdirs();
        return f;
    }

    static List<Driver> scan(File gameDir) {
        List<Driver> r = new ArrayList<>();
        File[] fs = folder(gameDir).listFiles();
        if (fs == null) return r;
        java.util.Arrays.sort(fs, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File f : fs) {
            String n = f.getName().toLowerCase(Locale.ROOT);
            if (n.startsWith(".")) continue;
            Driver d = null;
            try {
                if (f.isDirectory()) d = fromFolder(f);
                else if (n.endsWith(".zip") || n.endsWith(".adpkg")) d = fromZip(f);
                else if (n.endsWith(".so")) d = bare(f);
            } catch (Exception e) { d = new Driver(); d.src = f; d.name = f.getName(); d.error = L.t("не читается: ") + e.getMessage(); }
            if (d == null) continue;
            d.id = sanitize(f.getName().replaceAll("(?i)\\.(zip|adpkg)$", ""));
            if (d.name.isEmpty()) d.name = f.getName();
            if (d.error == null && d.libraryName.isEmpty()) d.error = L.t("в пакете нет Vulkan-библиотеки (.so)");
            r.add(d);
        }
        return r;
    }
    static String sanitize(String s) {
        String r = s.replaceAll("[^A-Za-z0-9._-]", "_");
        return r.isEmpty() || r.startsWith(".") ? "drv" + r : r;
    }
    private static void meta(Driver d, String json) {
        java.util.Map<String, String> o = flatJson(json);
        d.name = get(o, "name"); d.description = get(o, "description"); d.author = get(o, "author");
        d.packageVersion = get(o, "packageVersion"); d.vendor = get(o, "vendor"); d.driverVersion = get(o, "driverVersion");
        d.libraryName = get(o, "libraryName");
        try { d.minApi = (int) Double.parseDouble(get(o, "minApi")); } catch (Exception e) { d.minApi = 0; }
        if (d.libraryName.contains("/")) d.libraryName = d.libraryName.substring(d.libraryName.lastIndexOf('/') + 1);
    }
    private static String get(java.util.Map<String, String> o, String k) { String v = o.get(k); return v == null ? "" : v.trim(); }
    /** top-level "key": value pairs of a JSON object (strings, numbers, booleans; nested values skipped) - meta.json is flat */
    static java.util.Map<String, String> flatJson(String s) {
        java.util.Map<String, String> r = new java.util.HashMap<>();
        int i = s.indexOf('{') + 1, n = s.length(), depth = 0;
        if (i <= 0) return r;
        String key = null;
        while (i < n) {
            char c = s.charAt(i);
            if (c == '"') {
                StringBuilder b = new StringBuilder(); i++;
                while (i < n && s.charAt(i) != '"') {
                    char x = s.charAt(i++);
                    if (x == '\\' && i < n) {
                        char e = s.charAt(i++);
                        if (e == 'n') b.append('\n'); else if (e == 't') b.append('\t'); else if (e == 'r') b.append('\r');
                        else if (e == 'u' && i + 4 <= n) { try { b.append((char) Integer.parseInt(s.substring(i, i + 4), 16)); } catch (Exception ignored) { } i += 4; }
                        else b.append(e);
                    } else b.append(x);
                }
                i++;
                if (depth == 0) { if (key == null) key = b.toString(); else { r.put(key, b.toString()); key = null; } }
            } else if (c == '{' || c == '[') { depth++; i++; }
            else if (c == '}' || c == ']') { if (depth == 0) break; if (--depth == 0) key = null; i++; }
            else if (c == ',' && depth == 0) { key = null; i++; }
            else if (depth == 0 && key != null && c != ':' && !Character.isWhitespace(c)) {
                int j = i; while (j < n && ",}] \t\r\n".indexOf(s.charAt(j)) < 0) j++;
                r.put(key, s.substring(i, j)); key = null; i = j;
            } else i++;
        }
        return r;
    }
    private static Driver bare(File f) {
        Driver d = new Driver(); d.src = f; d.name = f.getName(); d.libraryName = f.getName();
        return d;
    }
    private static Driver fromFolder(File dir) throws Exception {
        File root = packageRoot(dir);
        Driver d = new Driver(); d.src = dir;
        File m = child(root, "meta.json");
        if (m != null) meta(d, readText(new FileInputStream(m)));
        if (d.libraryName.isEmpty() || child(root, d.libraryName) == null) {
            String lib = guessLib(names(root));
            if (lib == null) { if (m == null) return null; d.error = L.t("нет файла ") + (d.libraryName.isEmpty() ? ".so" : d.libraryName); return d; }
            d.libraryName = lib;
        }
        return d;
    }
    /** the folder holding meta.json / the .so files (archives often add one more level) */
    private static File packageRoot(File dir) {
        if (child(dir, "meta.json") != null || guessLib(names(dir)) != null) return dir;
        File[] sub = dir.listFiles();
        if (sub != null) for (File s : sub) if (s.isDirectory() && (child(s, "meta.json") != null || guessLib(names(s)) != null)) return s;
        return dir;
    }
    private static List<String> names(File dir) {
        List<String> r = new ArrayList<>(); File[] fs = dir.listFiles();
        if (fs != null) for (File f : fs) if (f.isFile()) r.add(f.getName());
        return r;
    }
    private static File child(File dir, String name) {
        File[] fs = dir.listFiles();
        if (fs != null) for (File f : fs) if (f.isFile() && f.getName().equalsIgnoreCase(name)) return f;
        return null;
    }
    /** a Vulkan ICD among the files: vulkan.*.so / libvulkan_*.so / any .so with "vulkan" in the name, else the only .so */
    static String guessLib(List<String> names) {
        String any = null; int so = 0;
        for (String n : names) {
            String l = n.toLowerCase(Locale.ROOT);
            if (!l.endsWith(".so")) continue;
            so++; any = n;
            if (l.startsWith("vulkan.") || l.startsWith("libvulkan_") || l.startsWith("libvulkan-")) return n;
        }
        for (String n : names) if (n.toLowerCase(Locale.ROOT).endsWith(".so") && n.toLowerCase(Locale.ROOT).contains("vulkan")) return n;
        return so == 1 ? any : null;
    }
    private static Driver fromZip(File f) throws Exception {
        Driver d = new Driver(); d.src = f;
        try (ZipFile z = new ZipFile(f)) {
            String prefix = zipPrefix(z);
            ZipEntry m = null; List<String> names = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement(); if (e.isDirectory() || !e.getName().startsWith(prefix)) continue;
                String rest = e.getName().substring(prefix.length()); if (rest.contains("/")) continue;
                if (rest.equalsIgnoreCase("meta.json")) m = e; else names.add(rest);
            }
            if (m != null) try (InputStream in = z.getInputStream(m)) { meta(d, readText(in)); }
            boolean has = false; for (String n : names) if (n.equalsIgnoreCase(d.libraryName)) { d.libraryName = n; has = true; }
            if (!has) {
                String lib = guessLib(names);
                if (lib == null) { d.error = L.t("нет файла ") + (d.libraryName.isEmpty() ? ".so" : d.libraryName) + L.t(" в архиве"); return d; }
                d.libraryName = lib;
            }
        }
        return d;
    }
    /** "" or "folder/" when everything (meta.json / .so) sits in one top folder of the archive */
    private static String zipPrefix(ZipFile z) {
        String best = null;
        for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
            ZipEntry e = en.nextElement(); if (e.isDirectory()) continue;
            String n = e.getName(), l = n.toLowerCase(Locale.ROOT);
            if (!l.endsWith("meta.json") && !l.endsWith(".so")) continue;
            int s = n.lastIndexOf('/'); String p = s < 0 ? "" : n.substring(0, s + 1);
            if (best == null || p.length() < best.length()) best = p;
        }
        return best == null ? "" : best;
    }
    private static String readText(InputStream in) throws IOException {
        try (InputStream i = in) {
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[8192]; int n;
            while ((n = i.read(buf)) > 0) { b.write(buf, 0, n); if (b.size() > 1 << 20) break; }
            String s = b.toString("UTF-8");
            return s.startsWith("\uFEFF") ? s.substring(1) : s;
        }
    }

    /** "" if the package suits this device, else a warning */
    static String compat(Driver d, String gpu, int sdk, String release) {
        StringBuilder w = new StringBuilder();
        if (d.minApi > sdk)
            w.append(L.t("Нужен Android API ")).append(d.minApi).append(" (").append(apiName(d.minApi)).append(L.t("), у вас ")).append(sdk)
             .append(" (Android ").append(release).append(L.t(") — драйвер, скорее всего, не загрузится."));
        String df = d.family(), gf = family(gpu);
        if (!df.isEmpty() && !gf.isEmpty() && !df.equals(gf))
            w.append(w.length() > 0 ? "\n" : "").append(L.t("Драйвер для ")).append(familyName(df)).append(L.t(", а у вас ")).append(familyName(gf))
             .append(L.t(" — с этим GPU он не заработает."));
        return w.toString();
    }
    static String apiName(int api) {
        String[] n = { "29", "10", "30", "11", "31", "12", "32", "12L", "33", "13", "34", "14", "35", "15", "36", "16" };
        for (int i = 0; i < n.length; i += 2) if (n[i].equals(String.valueOf(api))) return "Android " + n[i + 1];
        return api > 36 ? "Android 16+" : "Android ≤ 9";
    }

    /** unpacks the package into <base>/<id>/ (reused while the source file is unchanged); returns the driver library */
    static File install(File base, Driver d) throws IOException {
        File dir = new File(base, d.id), stamp = new File(dir, ".me1stamp");
        String want = d.src.getAbsolutePath() + "\n" + sizeAndTime(d.src) + "\n" + d.libraryName;
        File lib = new File(dir, d.libraryName);
        if (lib.isFile() && stamp.isFile() && want.equals(readText(new FileInputStream(stamp)))) return lib;
        deleteTree(dir); dir.mkdirs();
        if (!dir.isDirectory()) throw new IOException(L.t("не создаётся ") + dir);
        String n = d.src.getName().toLowerCase(Locale.ROOT);
        if (d.src.isDirectory()) {
            File root = packageRoot(d.src); File[] fs = root.listFiles();
            if (fs != null) for (File f : fs) if (f.isFile()) try (InputStream in = new FileInputStream(f)) { copy(in, new File(dir, f.getName())); }
        } else if (n.endsWith(".so")) {
            try (InputStream in = new FileInputStream(d.src)) { copy(in, lib); }
        } else try (ZipFile z = new ZipFile(d.src)) {
            String prefix = zipPrefix(z);
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement(); if (e.isDirectory() || !e.getName().startsWith(prefix)) continue;
                String rest = e.getName().substring(prefix.length());
                if (rest.isEmpty() || rest.contains("/") || rest.contains("\\") || rest.startsWith(".")) continue;
                try (InputStream in = z.getInputStream(e)) { copy(in, new File(dir, rest)); }
            }
        }
        if (!lib.isFile()) throw new IOException(L.t("после распаковки нет ") + d.libraryName);
        try (OutputStream o = new FileOutputStream(stamp)) { o.write(want.getBytes("UTF-8")); }
        return lib;
    }
    /** removes unpacked drivers whose package is gone from GameExDriver */
    static void cleanup(File base, List<Driver> keep) {
        File[] fs = base.listFiles(); if (fs == null) return;
        for (File f : fs) {
            boolean used = false; for (Driver d : keep) if (d.id.equals(f.getName())) used = true;
            if (!used) deleteTree(f);
        }
    }
    private static String sizeAndTime(File f) {
        if (!f.isDirectory()) return f.length() + ":" + f.lastModified();
        long s = 0, t = f.lastModified(); File[] fs = packageRoot(f).listFiles();
        if (fs != null) for (File c : fs) { s += c.length(); t = Math.max(t, c.lastModified()); }
        return s + ":" + t;
    }
    private static void copy(InputStream in, File to) throws IOException {
        File tmp = new File(to.getPath() + ".part");
        try (OutputStream o = new FileOutputStream(tmp)) { byte[] b = new byte[1 << 16]; int n; while ((n = in.read(b)) > 0) o.write(b, 0, n); }
        if (!tmp.renameTo(to)) throw new IOException(L.t("не записывается ") + to);
        to.setReadable(true, false);
    }
    static void deleteTree(File f) {
        File[] fs = f.listFiles(); if (fs != null) for (File c : fs) deleteTree(c);
        f.delete();
    }
}
