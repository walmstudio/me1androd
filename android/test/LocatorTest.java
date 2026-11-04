import com.me1port.launcher.GameLocator;
import java.io.File;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Collections;

/** Host test of GameLocator: detection order, nested folders, case-insensitivity, documents choice, migration, move. */
public class LocatorTest {
    static int fail = 0;
    static void check(boolean c, String what) { System.out.println((c ? "ok   " : "FAIL ") + what); if (!c) fail++; }
    static void touch(File f) throws Exception { f.getParentFile().mkdirs(); Files.write(f.toPath(), "x".getBytes()); }
    static void rm(File f) { File[] k = f.listFiles(); if (k != null) for (File c : k) rm(c); f.delete(); }
    public static void main(String[] a) throws Exception {
        File t = Files.createTempDirectory("loc").toFile();
        File sd = new File(t, "sdcard"), sdc = new File(t, "XXXX-XXXX"), priv = new File(sd, "Android/data/com.me1port.launcher/files");
        sd.mkdirs(); priv.mkdirs();
        // 1. nothing anywhere -> default /sdcard/MassEffect, docs inside it
        GameLocator L = GameLocator.locate(Arrays.asList(sd, sdc), priv, null);
        check(!L.found && L.gameDir.equals(new File(sd, "MassEffect")) && L.docsDir.equals(new File(sd, "MassEffect/Documents")), "empty -> default " + L.gameDir);
        // 2. old private location with settings -> found there, flagged legacy; old settings migrated
        touch(new File(priv, "MassEffect/Binaries/MassEffect.exe"));
        touch(new File(priv, "Documents/BioWare/Mass Effect/Config/me1port_overrides.ini"));
        rm(new File(sd, "MassEffect"));
        L = GameLocator.locate(Arrays.asList(sd, sdc), priv, null);
        check(L.found && L.legacyPrivate && L.gameDir.equals(new File(priv, "MassEffect")), "legacy private game found");
        check(L.migrated != null && new File(L.docsDir, "BioWare/Mass Effect/Config/me1port_overrides.ini").exists(), "settings migrated: " + L.migrated);
        // 3. move to /sdcard/MassEffect
        String err = GameLocator.moveGame(L.gameDir, new File(sd, "MassEffect"));
        check(err == null && new File(sd, "MassEffect/Binaries/MassEffect.exe").exists(), "moveGame -> " + err);
        L = GameLocator.locate(Arrays.asList(sd, sdc), priv, null);
        check(L.found && !L.legacyPrivate && L.gameDir.equals(new File(sd, "MassEffect")), "after move: /sdcard/MassEffect");
        check(new File(L.docsDir, "BioWare/Mass Effect/Config/me1port_overrides.ini").exists(), "settings moved with the game");
        // 4. nested + case variants: /sdcard/mass effect/Mass Effect/binaries/masseffect.exe
        rm(new File(sd, "MassEffect"));
        touch(new File(sd, "mass effect/Mass Effect/binaries/masseffect.exe"));
        L = GameLocator.locate(Arrays.asList(sd, sdc), priv, null);
        check(L.found && L.gameDir.equals(new File(sd, "mass effect/Mass Effect")), "nested + case-insensitive: " + L.gameDir);
        // 5. /sdcard/MassEffect wins over other names
        touch(new File(sd, "MassEffect/BioGame/Config/DefaultEngine.ini"));
        L = GameLocator.locate(Arrays.asList(sd, sdc), priv, null);
        check(L.gameDir.equals(new File(sd, "MassEffect")), "MassEffect preferred");
        // 6. SD card, read-only -> docs on /sdcard/MassEffect/Documents
        rm(new File(sd, "MassEffect")); rm(new File(sd, "mass effect"));
        touch(new File(sdc, "Mass_Effect/Binaries/MassEffect.exe"));
        new File(sdc, "Mass_Effect").setWritable(false);
        L = GameLocator.locate(Arrays.asList(sd, sdc), priv, null);
        check(L.found && L.gameDir.equals(new File(sdc, "Mass_Effect")), "SD card game: " + L.gameDir);
        File probe = new File(sdc, "Mass_Effect/.probe"); boolean ro = false;
        try { ro = !probe.createNewFile(); } catch (Exception e) { ro = true; } probe.delete();
        check(!ro || L.docsDir.equals(new File(sd, "MassEffect/Documents")), "read-only game -> docs " + L.docsDir + (ro ? "" : " (skipped: this FS ignores permissions)"));
        new File(sdc, "Mass_Effect").setWritable(true);
        // 7. manual choice
        touch(new File(t, "games/ME1/Binaries/MassEffect.exe"));
        L = GameLocator.locate(Arrays.asList(sd, sdc), priv, new File(t, "games/ME1").getPath());
        check(L.manual && L.found && L.gameDir.equals(new File(t, "games/ME1")), "manual path");
        L = GameLocator.locate(Collections.singletonList(sd), priv, new File(t, "nope").getPath());
        check(L.manual && !L.found, "manual path without game -> not found");
        rm(t);
        System.out.println(fail == 0 ? "LocatorTest: all ok" : "LocatorTest: " + fail + " failed");
        System.exit(fail == 0 ? 0 : 1);
    }
}
