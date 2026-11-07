package com.me1port.launcher;

/** JNI bridge to libme1port.so (settings + runtime self-check). */
public final class Native {
    static { System.loadLibrary("me1port"); }
    private Native() {}
    /** JSON: {"settings":[{id,group,label,help,type,min,max,choices:[[value,label]...]}], "presets":[{id,label,help}]} */
    public static native String schema();
    /** loads all settings; returns "id=value\tsource\n" lines */
    public static native String load(String gameDir, String docsDir);
    public static native String dump();
    /** Refresh saved values without discarding unsaved edits. */
    public static native String refreshClean();
    /** "" on success, otherwise an error message */
    public static native String set(String id, String value);
    public static native String preset(String id);
    /** automatic preset by device: "tier\nreport" (tier 0..4 = minimal..ultra, -1 error); apply=0 only classifies */
    public static native String autoPreset(String gpu, String soc, int ramMb, int cores, int maxMhz, int screenW, int screenH, int apply);
    public static native void reset();
    /** "OK\n..." or "ERR\n..." */
    public static native String save();
    /** bit 0 DefaultEngine.ini, 1 MassEffect.exe, 2 CookedPC, 3 PhysXCore.dll */
    public static native int gameStatus(String gameDir, String docsDir);
    /** human-readable report: files found and what the game will read (via the runtime's Win32 file layer) */
    public static native String check(String gameDir, String docsDir);
    public static native String portOption(String docsDir, String id);
}
