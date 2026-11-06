package com.me1port.launcher;

import android.view.KeyEvent;
import java.util.ArrayList;
import java.util.HashMap;

/** v1.2: what an element of the custom layout (CustomControls) sends. Names as in Winlator input-control profiles
 * (.icp): KEY_* (PC keys, sent as Android keycodes through GameActivity.nativeKey), MOUSE_* (buttons, wheel, motion),
 * GAMEPAD_* (the on-screen Xbox pad, GameActivity.pad: the game's XInput pad with the "xinput" profile, else Pad's
 * keyboard + mouse emulation); SPECIAL_KEYBOARD (the phone keyboard) is ours. Unknown names act as NONE but are kept. */
final class Bind {
    static final int NONE = 0, KEY = 1, MOUSE = 2, WHEEL = 3, MOVE = 4, PAD = 5, TRIG = 6, LSTICK = 7, RSTICK = 8, KBD = 9;
    /** categories of the binding picker (Russian source strings, L.t) */
    static final String[] CATS = { "Мышь", "Буквы A–Z", "Цифры 0–9", "F1–F12", "Служебные клавиши", "Стрелки и навигация",
            "Цифровой блок", "Символы", "Геймпад Xbox", "Особые" };
    final String name; final int kind, code, cat; private final String label, desc;
    private Bind(String name, int kind, int code, String label, String desc, int cat) {
        this.name = name; this.kind = kind; this.code = code; this.label = label; this.desc = desc; this.cat = cat;
    }
    static final ArrayList<Bind> ALL = new ArrayList<>();
    private static final HashMap<String, Bind> BY = new HashMap<>();
    static final Bind NO = new Bind("NONE", NONE, 0, "", "Нет", -1);

    private static void add(String name, int kind, int code, String label, String desc, int cat) {
        Bind b = new Bind(name, kind, code, label, desc, cat); ALL.add(b); BY.put(name, b);
    }
    private static void key(String name, int code, String label, String desc, int cat) { add(name, KEY, code, label, desc, cat); }
    private static void alias(String a, String name) { BY.put(a, BY.get(name)); }
    static {
        BY.put("NONE", NO);
        add("MOUSE_LEFT_BUTTON", MOUSE, 0, "ЛКМ", "Левая кнопка мыши", 0);
        add("MOUSE_RIGHT_BUTTON", MOUSE, 1, "ПКМ", "Правая кнопка мыши", 0);
        add("MOUSE_MIDDLE_BUTTON", MOUSE, 2, "СКМ", "Средняя кнопка мыши", 0);
        add("MOUSE_SCROLL_UP", WHEEL, 1, "Колесо ↑", "Колесо мыши вверх", 0);
        add("MOUSE_SCROLL_DOWN", WHEEL, -1, "Колесо ↓", "Колесо мыши вниз", 0);
        add("MOUSE_MOVE_UP", MOVE, 0, "Мышь ↑", "Движение мыши вверх", 0);
        add("MOUSE_MOVE_RIGHT", MOVE, 1, "Мышь →", "Движение мыши вправо", 0);
        add("MOUSE_MOVE_DOWN", MOVE, 2, "Мышь ↓", "Движение мыши вниз", 0);
        add("MOUSE_MOVE_LEFT", MOVE, 3, "Мышь ←", "Движение мыши влево", 0);
        for (char ch = 'A'; ch <= 'Z'; ch++) key("KEY_" + ch, KeyEvent.KEYCODE_A + (ch - 'A'), String.valueOf(ch), null, 1);
        for (int i = 0; i <= 9; i++) key("KEY_" + i, KeyEvent.KEYCODE_0 + i, String.valueOf(i), null, 2);
        for (int i = 1; i <= 12; i++) key("KEY_F" + i, KeyEvent.KEYCODE_F1 + i - 1, "F" + i, null, 3);
        key("KEY_ESC", KeyEvent.KEYCODE_ESCAPE, "Esc", null, 4);
        key("KEY_ENTER", KeyEvent.KEYCODE_ENTER, "Enter", null, 4);
        key("KEY_SPACE", KeyEvent.KEYCODE_SPACE, "Пробел", null, 4);
        key("KEY_TAB", KeyEvent.KEYCODE_TAB, "Tab", null, 4);
        key("KEY_BKSP", KeyEvent.KEYCODE_DEL, "⌫", "Backspace", 4);
        key("KEY_SHIFT_L", KeyEvent.KEYCODE_SHIFT_LEFT, "Shift", "Shift (левый)", 4);
        key("KEY_SHIFT_R", KeyEvent.KEYCODE_SHIFT_RIGHT, "RShift", "Shift (правый)", 4);
        key("KEY_CTRL_L", KeyEvent.KEYCODE_CTRL_LEFT, "Ctrl", "Ctrl (левый)", 4);
        key("KEY_CTRL_R", KeyEvent.KEYCODE_CTRL_RIGHT, "RCtrl", "Ctrl (правый)", 4);
        key("KEY_ALT_L", KeyEvent.KEYCODE_ALT_LEFT, "Alt", "Alt (левый)", 4);
        key("KEY_ALT_R", KeyEvent.KEYCODE_ALT_RIGHT, "RAlt", "Alt (правый)", 4);
        key("KEY_CAPS_LOCK", KeyEvent.KEYCODE_CAPS_LOCK, "Caps", "Caps Lock", 4);
        key("KEY_GRAVE", KeyEvent.KEYCODE_GRAVE, "`", "` ~ (консоль)", 4);
        key("KEY_UP", KeyEvent.KEYCODE_DPAD_UP, "↑", "Стрелка вверх", 5);
        key("KEY_RIGHT", KeyEvent.KEYCODE_DPAD_RIGHT, "→", "Стрелка вправо", 5);
        key("KEY_DOWN", KeyEvent.KEYCODE_DPAD_DOWN, "↓", "Стрелка вниз", 5);
        key("KEY_LEFT", KeyEvent.KEYCODE_DPAD_LEFT, "←", "Стрелка влево", 5);
        key("KEY_INSERT", KeyEvent.KEYCODE_INSERT, "Ins", "Insert", 5);
        key("KEY_DEL", KeyEvent.KEYCODE_FORWARD_DEL, "Del", "Delete", 5);
        key("KEY_HOME", KeyEvent.KEYCODE_MOVE_HOME, "Home", null, 5);
        key("KEY_END", KeyEvent.KEYCODE_MOVE_END, "End", null, 5);
        key("KEY_PG_UP", KeyEvent.KEYCODE_PAGE_UP, "PgUp", "Page Up", 5);
        key("KEY_PG_DOWN", KeyEvent.KEYCODE_PAGE_DOWN, "PgDn", "Page Down", 5);
        key("KEY_PRTSCN", KeyEvent.KEYCODE_SYSRQ, "PrtSc", "Print Screen", 5);
        key("KEY_PAUSE", KeyEvent.KEYCODE_BREAK, "Pause", null, 5);
        for (int i = 0; i <= 9; i++) key("KEY_KP_" + i, KeyEvent.KEYCODE_NUMPAD_0 + i, "Num" + i, null, 6);
        key("KEY_KP_ADD", KeyEvent.KEYCODE_NUMPAD_ADD, "Num+", null, 6);
        key("KEY_KP_SUBTRACT", KeyEvent.KEYCODE_NUMPAD_SUBTRACT, "Num−", null, 6);
        key("KEY_KP_MULTIPLY", KeyEvent.KEYCODE_NUMPAD_MULTIPLY, "Num*", null, 6);
        key("KEY_KP_DIVIDE", KeyEvent.KEYCODE_NUMPAD_DIVIDE, "Num/", null, 6);
        key("KEY_KP_DEL", KeyEvent.KEYCODE_NUMPAD_DOT, "Num.", null, 6);
        key("KEY_KP_ENTER", KeyEvent.KEYCODE_NUMPAD_ENTER, "NumEnter", null, 6);
        key("KEY_NUM_LOCK", KeyEvent.KEYCODE_NUM_LOCK, "NumLk", "Num Lock", 6);
        key("KEY_MINUS", KeyEvent.KEYCODE_MINUS, "-", null, 7);
        key("KEY_EQUAL", KeyEvent.KEYCODE_EQUALS, "=", null, 7);
        key("KEY_BRACKET_LEFT", KeyEvent.KEYCODE_LEFT_BRACKET, "[", null, 7);
        key("KEY_BRACKET_RIGHT", KeyEvent.KEYCODE_RIGHT_BRACKET, "]", null, 7);
        key("KEY_BACKSLASH", KeyEvent.KEYCODE_BACKSLASH, "\\", null, 7);
        key("KEY_SEMICOLON", KeyEvent.KEYCODE_SEMICOLON, ";", null, 7);
        key("KEY_APOSTROPHE", KeyEvent.KEYCODE_APOSTROPHE, "'", null, 7);
        key("KEY_COMMA", KeyEvent.KEYCODE_COMMA, ",", null, 7);
        key("KEY_PERIOD", KeyEvent.KEYCODE_PERIOD, ".", null, 7);
        key("KEY_SLASH", KeyEvent.KEYCODE_SLASH, "/", null, 7);
        add("GAMEPAD_BUTTON_A", PAD, Pad.A, "A", null, 8);
        add("GAMEPAD_BUTTON_B", PAD, Pad.B, "B", null, 8);
        add("GAMEPAD_BUTTON_X", PAD, Pad.X, "X", null, 8);
        add("GAMEPAD_BUTTON_Y", PAD, Pad.Y, "Y", null, 8);
        add("GAMEPAD_BUTTON_L1", PAD, Pad.LB, "LB", null, 8);
        add("GAMEPAD_BUTTON_R1", PAD, Pad.RB, "RB", null, 8);
        add("GAMEPAD_BUTTON_L2", TRIG, 1, "LT", null, 8);
        add("GAMEPAD_BUTTON_R2", TRIG, 2, "RT", null, 8);
        add("GAMEPAD_BUTTON_SELECT", PAD, Pad.BACK, "Back", null, 8);
        add("GAMEPAD_BUTTON_START", PAD, Pad.START, "Start", null, 8);
        add("GAMEPAD_BUTTON_L3", PAD, Pad.LTHUMB, "L3", null, 8);
        add("GAMEPAD_BUTTON_R3", PAD, Pad.RTHUMB, "R3", null, 8);
        add("GAMEPAD_DPAD_UP", PAD, Pad.UP, "D↑", "Крестовина вверх", 8);
        add("GAMEPAD_DPAD_RIGHT", PAD, Pad.RIGHT, "D→", "Крестовина вправо", 8);
        add("GAMEPAD_DPAD_DOWN", PAD, Pad.DOWN, "D↓", "Крестовина вниз", 8);
        add("GAMEPAD_DPAD_LEFT", PAD, Pad.LEFT, "D←", "Крестовина влево", 8);
        String[] dn = { "UP", "RIGHT", "DOWN", "LEFT" }, da = { "↑", "→", "↓", "←" };
        String[] dl = { "Левый стик вверх", "Левый стик вправо", "Левый стик вниз", "Левый стик влево" },
                 dr = { "Правый стик вверх", "Правый стик вправо", "Правый стик вниз", "Правый стик влево" };
        for (int i = 0; i < 4; i++) add("GAMEPAD_LEFT_THUMB_" + dn[i], LSTICK, i, "LS" + da[i], dl[i], 8);
        for (int i = 0; i < 4; i++) add("GAMEPAD_RIGHT_THUMB_" + dn[i], RSTICK, i, "RS" + da[i], dr[i], 8);
        add("SPECIAL_KEYBOARD", KBD, 0, "⌨", "Клавиатура телефона", 9);
        for (int i = 0; i <= 9; i++) { alias("KEY_NP_" + i, "KEY_KP_" + i); alias("KEY_NP" + i, "KEY_KP_" + i); alias("KEY_NUMPAD_" + i, "KEY_KP_" + i); }
        alias("KEY_ESCAPE", "KEY_ESC"); alias("KEY_RETURN", "KEY_ENTER"); alias("KEY_BACKSPACE", "KEY_BKSP"); alias("KEY_DELETE", "KEY_DEL");
        alias("KEY_SHIFT", "KEY_SHIFT_L"); alias("KEY_CTRL", "KEY_CTRL_L"); alias("KEY_ALT", "KEY_ALT_L"); alias("KEY_TILDE", "KEY_GRAVE");
        alias("KEY_KP_DEC", "KEY_KP_DEL"); alias("KEY_KP_PERIOD", "KEY_KP_DEL"); alias("KEY_PAGE_UP", "KEY_PG_UP"); alias("KEY_PAGE_DOWN", "KEY_PG_DOWN");
        alias("KEY_INS", "KEY_INSERT"); alias("KEY_CAPSLOCK", "KEY_CAPS_LOCK"); alias("KEY_EQUALS", "KEY_EQUAL");
    }
    static Bind of(String n) { Bind b = n == null ? null : BY.get(n); return b != null ? b : NO; }
    static ArrayList<Bind> inCat(int c) { ArrayList<Bind> r = new ArrayList<>(); for (Bind b : ALL) if (b.cat == c) r.add(b); return r; }
    /** short label on the element */
    String label() { return L.t(label); }
    /** longer name in the picker */
    String desc() { return desc != null ? L.t(desc) : label(); }
    boolean none() { return kind == NONE; }
}
