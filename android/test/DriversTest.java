package com.me1port.launcher;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Host test of GpuDrivers (package com.me1port.launcher, compiled together): scan of GameExDriver (zip / folder / bare .so,
 * nested archive folder, any letter case), meta.json parsing, compatibility warnings, unpacking + reuse + cleanup.
 * Optional args: real AdrenoTools packages (.zip) to scan as well. */
public class DriversTest {
    static int fail = 0;
    static void check(boolean c, String what) { System.out.println((c ? "ok   " : "FAIL ") + what); if (!c) fail++; }
    static void write(File f, String s) throws Exception { f.getParentFile().mkdirs(); Files.write(f.toPath(), s.getBytes("UTF-8")); }
    static void zip(File z, String... nameData) throws Exception {
        try (ZipOutputStream o = new ZipOutputStream(new FileOutputStream(z))) {
            for (int i = 0; i < nameData.length; i += 2) { o.putNextEntry(new ZipEntry(nameData[i])); o.write(nameData[i + 1].getBytes("UTF-8")); o.closeEntry(); }
        }
    }
    static GpuDrivers.Driver find(List<GpuDrivers.Driver> l, String file) { for (GpuDrivers.Driver d : l) if (d.src.getName().equals(file)) return d; return null; }
    public static void main(String[] a) throws Exception {
        File t = Files.createTempDirectory("drv").toFile(), game = new File(t, "MassEffect"), priv = new File(t, "files/gpu_drivers");
        game.mkdirs();
        check(GpuDrivers.scan(game).isEmpty() && new File(game, "GameExDriver").isDirectory(), "empty game -> GameExDriver created, nothing found");
        GpuDrivers.deleteTree(new File(game, "GameExDriver"));
        File dir = new File(game, "gameexdriver"); dir.mkdirs();
        check(GpuDrivers.folder(game).equals(dir), "folder name is case-insensitive");
        String meta = "{\n \"schemaVersion\": 1, \"name\": \"Turnip \\\"test\\\"\", \"description\": \"Mesa Turnip\\nfor Adreno 6xx\", \"author\": \"K11\",\n"
                + " \"packageVersion\": \"2\", \"vendor\": \"Mesa\", \"driverVersion\": \"25.1.0\", \"minApi\": 28, \"libraryName\": \"vulkan.ad07XX.so\", \"extra\": {\"a\": [1, 2]} }";
        zip(new File(dir, "turnip.zip"), "meta.json", meta, "vulkan.ad07XX.so", "ELF-turnip", "libdep.so", "dep");
        zip(new File(dir, "Nested Pkg.zip"), "pkg/meta.json", "{\"name\":\"N\",\"vendor\":\"Qualcomm\",\"driverVersion\":\"1\",\"minApi\":36,\"libraryName\":\"vulkan.adr1.so\"}",
                "pkg/vulkan.adr1.so", "q", "pkg/vkbgsl.so", "g", "pkg/../evil.so", "x");
        zip(new File(dir, "nometa.zip"), "libvulkan_freedreno.so", "f");
        zip(new File(dir, "broken.zip"), "readme.txt", "nothing");
        write(new File(dir, "mali/meta.json"), "{\"name\":\"malisx2 0.3\",\"vendor\":\"malisx2\",\"driverVersion\":\"44.1.0\",\"minApi\":29,\"libraryName\":\"libvulkan_malisx2.so\",\"description\":\"for Arm Mali-G52\"}");
        write(new File(dir, "mali/libvulkan_malisx2.so"), "m");
        write(new File(dir, "vulkan.custom.so"), "c");
        write(new File(dir, "notes.txt"), "ignored");
        List<GpuDrivers.Driver> l = GpuDrivers.scan(game);
        for (GpuDrivers.Driver d : l) System.out.println("     " + d.id + ": " + d.label() + " lib=" + d.libraryName + " fam=" + d.family() + (d.error != null ? " ERR " + d.error : ""));
        check(l.size() == 6, "6 entries (zip, nested zip, zip without meta, broken zip, folder, bare .so), got " + l.size());
        GpuDrivers.Driver tu = find(l, "turnip.zip"), ne = find(l, "Nested Pkg.zip"), nm = find(l, "nometa.zip"), br = find(l, "broken.zip"),
                ma = find(l, "mali"), so = find(l, "vulkan.custom.so");
        check(tu != null && tu.error == null && tu.name.equals("Turnip \"test\"") && tu.description.equals("Mesa Turnip\nfor Adreno 6xx") && tu.minApi == 28
                && tu.libraryName.equals("vulkan.ad07XX.so") && tu.family().equals("adreno") && tu.label().equals("Turnip \"test\" 25.1.0 (Mesa)"), "meta.json parsed: " + (tu == null ? null : tu.label()));
        check(ne != null && ne.error == null && ne.id.equals("Nested_Pkg") && ne.libraryName.equals("vulkan.adr1.so") && ne.minApi == 36, "archive with a top folder, id sanitized");
        check(nm != null && nm.error == null && nm.libraryName.equals("libvulkan_freedreno.so") && nm.family().equals("adreno"), "no meta.json -> library guessed");
        check(br != null && br.error != null, "archive without .so -> error: " + (br == null ? null : br.error));
        check(ma != null && ma.error == null && ma.family().equals("mali") && ma.libraryName.equals("libvulkan_malisx2.so"), "folder package");
        check(so != null && so.error == null && so.libraryName.equals("vulkan.custom.so") && so.family().isEmpty(), "bare .so");
        // compatibility
        check(GpuDrivers.compat(tu, "Mali-G52 MC2", 29, "10").contains("Adreno"), "Adreno driver on Mali -> warning");
        check(GpuDrivers.compat(tu, "Adreno (TM) 610", 29, "10").isEmpty(), "Adreno driver on Adreno, API ok -> no warning");
        String w = GpuDrivers.compat(ne, "Adreno (TM) 740", 29, "10");
        check(w.contains("API 36") && w.contains("Android 16") && !w.contains("Mali"), "minApi 36 on Android 10 -> warning: " + w.replace('\n', ' '));
        check(GpuDrivers.compat(ma, "Mali-G52 MC2", 29, "10").isEmpty() && !GpuDrivers.compat(ma, "Adreno (TM) 619", 29, "10").isEmpty(), "Mali driver: ok on Mali, warning on Adreno");
        check(GpuDrivers.compat(so, "Mali-G52", 29, "10").isEmpty(), "unknown vendor -> no warning");
        // install
        File lib = GpuDrivers.install(priv, ne);
        check(lib.equals(new File(priv, "Nested_Pkg/vulkan.adr1.so")) && new File(priv, "Nested_Pkg/vkbgsl.so").isFile()
                && !new File(priv, "evil.so").exists() && !new File(priv, "Nested_Pkg/meta.json").exists() == false, "zip unpacked flat (deps next to the driver, no ../)");
        long m0 = lib.lastModified(); Thread.sleep(1100);
        check(GpuDrivers.install(priv, ne).lastModified() == m0, "unchanged package -> reused");
        zip(new File(dir, "Nested Pkg.zip"), "pkg/meta.json", "{\"name\":\"N\",\"libraryName\":\"vulkan.adr1.so\"}", "pkg/vulkan.adr1.so", "q2");
        ne = find(GpuDrivers.scan(game), "Nested Pkg.zip");
        lib = GpuDrivers.install(priv, ne);
        check(new String(Files.readAllBytes(lib.toPath())).equals("q2") && !new File(priv, "Nested_Pkg/vkbgsl.so").exists(), "changed package -> unpacked again, old files gone");
        check(GpuDrivers.install(priv, ma).equals(new File(priv, "mali/libvulkan_malisx2.so")), "folder installed");
        check(GpuDrivers.install(priv, so).equals(new File(priv, "vulkan.custom.so/vulkan.custom.so")), "bare .so installed");
        new File(dir, "vulkan.custom.so").delete();
        GpuDrivers.cleanup(priv, GpuDrivers.scan(game));
        check(!new File(priv, "vulkan.custom.so").exists() && new File(priv, "mali").isDirectory(), "cleanup removes unpacked drivers of deleted packages");
        try { GpuDrivers.install(priv, br); check(false, "broken package install must fail"); } catch (Exception e) { check(true, "broken package install fails: " + e.getMessage()); }
        // real packages from the command line
        for (String p : a) {
            File f = new File(p); File d2 = new File(t, "real/MassEffect/GameExDriver"); d2.mkdirs();
            Files.copy(f.toPath(), new File(d2, f.getName()).toPath());
            GpuDrivers.Driver d = find(GpuDrivers.scan(new File(t, "real/MassEffect")), f.getName());
            check(d != null && d.error == null, "real " + f.getName() + ": " + (d == null ? null : d.label() + " lib=" + d.libraryName + " minApi=" + d.minApi + " fam=" + d.family()
                    + " | on Mali-G52/Android 10: " + GpuDrivers.compat(d, "Mali-G52 MC2", 29, "10").replace('\n', ' ')));
            if (d != null && d.error == null) {
                File li = GpuDrivers.install(new File(t, "real/priv"), d);
                check(li.isFile() && li.length() > 100000, "real " + f.getName() + " unpacked: " + li.getName() + " " + li.length() + " B, files " + li.getParentFile().list().length);
            }
        }
        GpuDrivers.deleteTree(t);
        System.out.println(fail == 0 ? "ALL OK" : fail + " FAILED");
        System.exit(fail == 0 ? 0 : 1);
    }
}
