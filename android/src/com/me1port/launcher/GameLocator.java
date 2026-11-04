package com.me1port.launcher;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Finds the game folder and picks where saves/settings live. Pure java.io (no Android API) so it is host-testable.
 *
 * Search order: manual choice → <storage root>/MassEffect → other root folders named "Mass Effect*" (any case/spaces) on
 * internal storage and SD cards → the old app-private folder (Android/data/…/files/MassEffect). Each candidate may hold
 * the game directly or one level deeper (/sdcard/MassEffect/Mass Effect/Binaries/…).
 * Documents ("Мои документы" for the game: BioWare/Mass Effect/{Config,Save}) = <game>/Documents when writable,
 * else /sdcard/MassEffect/Documents, else app-private. Settings from the old location are copied over once.
 */
public final class GameLocator {
    public static final String DEFAULT_NAME = "MassEffect";
    public File gameDir, docsDir;
    public boolean found, manual, legacyPrivate;
    public final List<File> searched = new ArrayList<>();
    public String migrated;

    /** roots[0] = primary shared storage (/sdcard), the rest = SD cards; priv = app-specific external dir */
    public static GameLocator locate(List<File> roots, File priv, String manualPath) {
        GameLocator L = new GameLocator();
        File primary = roots.isEmpty() ? null : roots.get(0);
        if (manualPath != null && !manualPath.trim().isEmpty()) {
            File m = new File(manualPath.trim()); L.manual = true; L.searched.add(m);
            File g = findGame(m); L.found = g != null; L.gameDir = g != null ? g : m;
        } else {
            outer:
            for (File r : roots) for (File c : candidates(r)) {
                L.searched.add(c); File g = findGame(c);
                if (g != null) { L.gameDir = g; L.found = true; break outer; }
            }
            if (!L.found && priv != null) {
                File c = new File(priv, DEFAULT_NAME); File g = findGame(c);
                if (g != null) { L.gameDir = g; L.found = true; L.legacyPrivate = true; L.searched.add(c); }
            }
            if (!L.found) L.gameDir = new File(primary != null ? primary : priv, DEFAULT_NAME);
        }
        if (writable(L.gameDir)) L.docsDir = new File(L.gameDir, "Documents");
        else if (primary != null && writable(new File(primary, DEFAULT_NAME))) L.docsDir = new File(new File(primary, DEFAULT_NAME), "Documents");
        else L.docsDir = new File(priv, "Documents");
        L.docsDir.mkdirs();
        if (priv != null) L.migrated = migrate(new File(priv, "Documents"), L.docsDir);
        return L;
    }

    static String norm(String s) { return s.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]", ""); }

    /** folders in the storage root that look like the game: MassEffect first, then "Mass Effect", "mass_effect_1", … */
    static List<File> candidates(File root) {
        List<File> out = new ArrayList<>();
        out.add(new File(root, DEFAULT_NAME));
        File[] k = root.listFiles();
        if (k != null) {
            Arrays.sort(k);
            for (File f : k) if (f.isDirectory() && !f.getName().equals(DEFAULT_NAME) && norm(f.getName()).startsWith("masseffect")) out.add(f);
        }
        return out;
    }

    /** case-insensitive child lookup (the game's file layer is case-insensitive too) */
    static File child(File dir, String name) {
        File f = new File(dir, name);
        if (f.exists()) return f;
        String[] k = dir.list();
        if (k != null) for (String s : k) if (s.equalsIgnoreCase(name)) return new File(dir, s);
        return null;
    }
    static boolean has(File dir, String... path) {
        File f = dir;
        for (String p : path) { if (f == null) return false; f = child(f, p); }
        return f != null;
    }
    public static boolean isGame(File d) {
        return d != null && d.isDirectory() && (has(d, "Binaries", "MassEffect.exe") || has(d, "BioGame", "Config", "DefaultEngine.ini"));
    }
    /** the folder itself or a sub-folder one level down */
    public static File findGame(File d) {
        if (isGame(d)) return d;
        File[] k = d.listFiles();
        if (k == null) return null;
        Arrays.sort(k);
        for (File f : k) if (f.isDirectory() && !f.getName().startsWith(".") && !f.getName().equalsIgnoreCase("Documents") && isGame(f)) return f;
        return null;
    }
    static boolean writable(File d) {
        d.mkdirs();
        if (!d.isDirectory()) return false;
        File t = new File(d, ".me1port_write_test");
        try { t.delete(); if (!t.createNewFile()) return false; return t.delete(); } catch (IOException | SecurityException e) { return false; }
    }

    /** copies <old>/BioWare once if the new documents folder has no settings yet; the old copy is left in place */
    static String migrate(File oldDocs, File newDocs) {
        try {
            if (oldDocs.getCanonicalFile().equals(newDocs.getCanonicalFile())) return null;
        } catch (IOException e) { return null; }
        File src = new File(oldDocs, "BioWare"), dst = new File(newDocs, "BioWare");
        if (!src.isDirectory() || dst.exists() || countFiles(src) == 0) return null;
        try { int n = copyTree(src, dst); return L.t("Перенесены настройки (") + n + L.t(" файл.) из ") + oldDocs; }
        catch (IOException e) { return L.t("Не удалось перенести настройки из ") + oldDocs + ": " + e.getMessage(); }
    }
    static int countFiles(File d) {
        File[] k = d.listFiles(); int n = 0;
        if (k != null) for (File f : k) n += f.isDirectory() ? countFiles(f) : 1;
        return n;
    }
    static int copyTree(File src, File dst) throws IOException {
        if (src.isDirectory()) {
            if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("mkdir " + dst);
            int n = 0; File[] k = src.listFiles();
            if (k != null) for (File f : k) n += copyTree(f, new File(dst, f.getName()));
            return n;
        }
        try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(dst)) {
            byte[] b = new byte[65536]; int r;
            while ((r = in.read(b)) > 0) out.write(b, 0, r);
        }
        dst.setLastModified(src.lastModified());
        return 1;
    }

    /**
     * Moves the game out of the app-private folder (deleted on uninstall) into target (/sdcard/MassEffect) by renaming
     * each top-level entry — instant on the same storage, nothing is copied. Existing entries in target are kept.
     * Returns null on success or an error text.
     */
    public static String moveGame(File from, File target) {
        target.mkdirs();
        if (!target.isDirectory()) return L.t("Не удалось создать ") + target;
        File[] k = from.listFiles();
        if (k == null) return L.t("Нет доступа к ") + from;
        List<String> failed = new ArrayList<>();
        for (File f : k) {
            File t = new File(target, f.getName());
            if (t.exists()) {
                if (f.getName().equalsIgnoreCase("Documents")) migrate(f, t); else failed.add(f.getName() + L.t(" (уже есть)"));
                continue;
            }
            if (!f.renameTo(t)) failed.add(f.getName());
        }
        if (failed.isEmpty()) { String[] left = from.list(); if (left == null || left.length == 0) from.delete(); return null; }
        return L.t("Не перенесено: ") + failed + L.t(". Перенесите вручную файловым менеджером из\n") + from + L.t("\nв\n") + target;
    }
}
