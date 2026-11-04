import com.me1port.launcher.Native;
/** Host JVM test of the JNI bridge (x86-64 build of libme1port.so). */
public class HostTest {
    static int fail = 0;
    static void check(boolean c, String what) { System.out.println((c ? "ok   " : "FAIL ") + what); if (!c) fail++; }
    public static void main(String[] a) throws Exception {
        String game = a[0], docs = a[1];
        String schema = Native.schema();
        check(schema.startsWith("{\"settings\":[") && schema.contains("\"presets\""), "schema JSON (" + schema.length() + " bytes)");
        String d = Native.load(game, docs);
        check(d.contains("gfx.texture_detail=1\tdefault"), "load: defaults from DefaultEngine.ini");
        check(Native.set("gfx.texture_detail", "9").contains("недопустимое"), "set: validation message in Russian");
        check(Native.preset("low").isEmpty(), "preset low");
        check(Native.set("screen.gamma", "1.8").isEmpty(), "set gamma");
        String r = Native.save();
        check(r.startsWith("OK\n"), "save: " + r.replace('\n', ' '));
        d = Native.load(game, docs);
        check(d.contains("screen.resolution=960x540\toverride") && d.contains("screen.gamma=1.8\toverride"), "reload sees overrides");
        check(Native.gameStatus(game, docs) == 1, "gameStatus = " + Native.gameStatus(game, docs) + " (only configs present)");
        String c = Native.check(game, docs);
        System.out.println(c);
        check(c.contains("Разрешение X           960") && c.contains("Детализация текстур    0") && c.contains("DefaultEngine.ini + настройки порта"), "check(): game reads patched DefaultEngine.ini via thunks");
        check(Native.portOption(docs, "port.fps_limit").equals("30"), "portOption fps_limit");
        Native.reset(); r = Native.save(); d = Native.load(game, docs);
        check(d.contains("screen.resolution=1024x768\tdefault"), "reset + save -> defaults");
        System.out.println(fail == 0 ? "HostTest: all ok" : "HostTest: " + fail + " failed");
        System.exit(fail == 0 ? 0 : 1);
    }
}
