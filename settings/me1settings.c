#define _GNU_SOURCE
#include "me1settings.h"
#include "ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>

static const char* const FILE_NAME[SF_COUNT] = { "Engine", "Game", "Input", "Port" };

static const SetChoice RES[] = {
    {"640x360", "640×360"}, {"854x480", "854×480"}, {"960x540", "960×540"}, {"1024x576", "1024×576"},
    {"1280x720", "1280×720"}, {"1366x768", "1366×768"}, {"1600x900", "1600×900"}, {"1920x1080", "1920×1080"},
    {"2560x1440", "2560×1440"}, {"3840x2160", "3840×2160 (4K)"},
    {"1280x800", "1280×800 (16:10)"}, {"1440x900", "1440×900 (16:10)"}, {"1680x1050", "1680×1050 (16:10)"},
    {"1920x1200", "1920×1200 (16:10)"}, {"2560x1600", "2560×1600 (16:10)"},
    {"2560x1080", "2560×1080 (21:9)"}, {"3440x1440", "3440×1440 (21:9)"},
    {"800x600", "800×600 (4:3)"}, {"1024x768", "1024×768 (4:3)"}, {"1280x1024", "1280×1024 (5:4)"}, {0, 0} };
static const SetChoice TEXDETAIL[] = { {"0", "Низкая (до 512)"}, {"1", "Средняя (до 1024)"}, {"2", "Высокая (до 2048)"}, {"3", "Максимальная (до 4096)"}, {0, 0} };
static const SetChoice FILTER[] = { {"0", "Низкое"}, {"1", "Среднее"}, {"2", "Высокое"}, {0, 0} };
static const SetChoice PARTICLES[] = { {"0", "Высокая"}, {"1", "Средняя"}, {"2", "Низкая"}, {0, 0} };
static const SetChoice TEXSIZE[] = { {"512", "512"}, {"1024", "1024"}, {"2048", "2048"}, {"4096", "4096"}, {0, 0} };
static const SetChoice AUDIODEV[] = { {"Generic Software", "Программный (рекомендуется)"}, {"Generic Hardware", "Аппаратный"}, {"DirectSound3D", "DirectSound3D"}, {0, 0} };
static const SetChoice LANG[] = { {"RU", "Русский"}, {"INT", "Английский"}, {"DE", "Немецкий"}, {"FR", "Французский"}, {"IT", "Итальянский"},
    {"ES", "Испанский"}, {"PL", "Польский"}, {"CZ", "Чешский"}, {"HU", "Венгерский"}, {0, 0} };
static const SetChoice DIFF[] = { {"0", "Лёгкий"}, {"1", "Нормальный"}, {"2", "Ветеран"}, {"3", "Хардкор"}, {"4", "Безумие"}, {0, 0} };
static const SetChoice SCRPCT[] = { {"100", "100% (без масштабирования)"}, {"85", "85%"}, {"75", "75%"}, {"65", "65%"}, {"50", "50%"}, {0, 0} };
static const SetChoice FPSCAP[] = { {"30", "30 кадров/с"}, {"45", "45 кадров/с"}, {"60", "60 кадров/с"}, {"0", "Без ограничения"}, {0, 0} };

#define WC "WinDrv.WindowsClient"
const SettingDef me1_settings[] = {
    /* id                  group       label                       help */
    {"screen.resolution", "Экран", "Разрешение", "Внутреннее разрешение рендера; картинка масштабируется на весь экран",
        SF_ENGINE, WC, "StartupResolutionX", "StartupResolutionY", 0, ST_RES, "1024x768", 0, 0, RES},
    {"screen.fullscreen", "Экран", "Полноэкранный режим", "На Android всегда полный экран; влияет на команду setres",
        SF_ENGINE, WC, "StartupFullscreen", 0, 0, ST_BOOL, "1", 0, 0, 0},
    {"screen.vsync", "Экран", "Вертикальная синхронизация", "Убирает разрывы кадра",
        SF_ENGINE, WC, "UseVSync", 0, 0, ST_BOOL, "0", 0, 0, 0},
    {"screen.gamma", "Экран", "Гамма", "Яркость изображения (по умолчанию 1.6)",
        SF_ENGINE, "Engine.Client", "DisplayGamma", 0, 0, ST_FLOAT, "1.6", 1.0, 3.0, 0},
    {"gfx.texture_detail", "Графика", "Детализация текстур", "TextureLODLevel: максимальный размер текстур мира и персонажей",
        SF_ENGINE, WC, "TextureLODLevel", 0, 0, ST_ENUM, "1", 0, 3, TEXDETAIL},
    {"gfx.filter", "Графика", "Фильтрация текстур", "FilterLevel",
        SF_ENGINE, WC, "FilterLevel", 0, 0, ST_ENUM, "1", 0, 2, FILTER},
    {"gfx.dynamic_shadows", "Графика", "Динамические тени", "Самая тяжёлая опция для слабых GPU",
        SF_ENGINE, WC, "EnableDynamicShadows", 0, 0, ST_BOOL, "0", 0, 0, 0},
    {"gfx.particles", "Графика", "Детализация частиц", "ParticleLODBias",
        SF_ENGINE, "SystemSettings", "ParticleLODBias", 0, 0, ST_ENUM, "1", 0, 2, PARTICLES},
    {"gfx.post_process", "Графика", "Постобработка", "Bloom, глубина резкости и т. п.",
        SF_ENGINE, "Engine.GameEngine", "bUsePostProcessEffects", 0, "Engine.RenderDevice", ST_BOOL, "1", 0, 0, 0},
    /* v1.1.3: single effects (the switch above turns all post-processing off) */
    {"gfx.lens_flares", "Графика", "Блики линз", "LensFlares: блики и ореолы от солнца и ламп. Выключите, если они слишком яркие "
        "или мешают",
        SF_ENGINE, "SystemSettings", "LensFlares", 0, 0, ST_BOOL, "1", 0, 0, 0},
    {"gfx.bloom", "Графика", "Свечение (bloom)", "Bloom: ореол вокруг ярких мест. Выключите, если картинка в дымке или нужно "
        "больше FPS на слабой видеокарте",
        SF_ENGINE, "SystemSettings", "Bloom", 0, 0, ST_BOOL, "1", 0, 0, 0},
    {"gfx.max_texture_size", "Графика", "Макс. размер текстуры", "MaxTextureSize рендерера",
        SF_ENGINE, "D3DDrv.D3DRenderDevice", "MaxTextureSize", 0, 0, ST_ENUM, "2048", 0, 0, TEXSIZE},
    {"gfx.screen_percentage", "Графика", "Масштаб 3D-сцены", "ScreenPercentage: 3D-сцена рисуется в меньшем разрешении и растягивается, "
        "интерфейс и текст остаются чёткими. 75% при 1280×720 — сцена 960×540: заметно меньше нагрузка на GPU. Экспериментально: "
        "если картинка окажется не на весь экран, верните 100%",
        SF_ENGINE, "SystemSettings", "ScreenPercentage", 0, 0, ST_ENUM, "100", 0, 0, SCRPCT},
    {"port.simple_shaders", "Графика", "Упрощённый расчёт", "Облегчённый режим для слабых устройств. Освещение и эффекты движка: без "
        "динамических теней и теней персонажей, простые "
        "карты освещения, без размытия в движении, глубины резкости, бликов линз и динамических декалей, упрощённый bloom, "
        "билинейная фильтрация, персонажи на одну ступень LOD ниже, без мелких деталей окружения. Шейдеры в половинной "
        "точности (FP16) там, где игра допускает half, дешёвое sRGB; звуковой таймер 5 мс и звук 32 кГц. Ваши настройки в "
        "BIOEngine.ini не меняются — режим применяется в памяти и выключается без следов. Быстрее, картинка заметно проще",
        SF_PORT, "Port", "SimpleShaders", 0, 0, ST_BOOL01, "0", 0, 0, 0},
    {"snd.device", "Звук", "Звуковое устройство", "Устройство ISACT; в порте звук идёт через OpenAL Soft",
        SF_ENGINE, "ISACTAudio.ISACTAudioDevice", "DeviceName", 0, 0, ST_ENUM, "Generic Software", 0, 0, AUDIODEV},
    {"snd.channels", "Звук", "Каналов звука", "Одновременно звучащих источников (меньше — быстрее)",
        SF_ENGINE, "ISACTAudio.ISACTAudioDevice", "MaxChannels", "TotalAvailableChannels", 0, ST_INT, "64", 16, 64, 0},
    {"game.language", "Игра", "Язык текста", "Language в [Engine.Engine]; нужны соответствующие файлы локализации",
        SF_ENGINE, "Engine.Engine", "Language", 0, 0, ST_ENUM, "RU", 0, 0, LANG},
    {"game.subtitles", "Игра", "Субтитры", "Значение по умолчанию для нового профиля",
        SF_GAME, "BIOC_Base.BioGamerProfile", "m_nDefaultSubTitles", 0, 0, ST_BOOL01, "1", 0, 0, 0},
    {"game.motion_blur", "Игра", "Размытие в движении", "Значение по умолчанию для нового профиля",
        SF_GAME, "BIOC_Base.BioGamerProfile", "m_nDefaultMotionBlur", 0, 0, ST_BOOL01, "1", 0, 0, 0},
    {"game.film_grain", "Игра", "Зернистость плёнки", "Значение по умолчанию для нового профиля",
        SF_GAME, "BIOC_Base.BioGamerProfile", "m_nDefaultFilmGrain", 0, 0, ST_BOOL01, "1", 0, 0, 0},
    {"game.difficulty", "Игра", "Сложность", "Значение по умолчанию для нового профиля",
        SF_GAME, "BIOC_Base.BioGamerProfile", "m_nDefaultCombatDifficulty", 0, 0, ST_ENUM, "1", 0, 4, DIFF},
    {"game.autosave", "Игра", "Автосохранение", "Значение по умолчанию для нового профиля",
        SF_GAME, "BIOC_Base.BioGamerProfile", "m_nDefaultAutoSave", 0, 0, ST_BOOL01, "1", 0, 0, 0},
    {"input.mouse_sensitivity", "Управление", "Чувствительность обзора", "MouseSensitivity (сенсорный обзор и мышь)",
        SF_INPUT, "Engine.PlayerInput", "MouseSensitivity", 0, 0, ST_FLOAT, "6", 1, 20, 0},
    {"port.fps_limit", "Android", "Ограничение FPS", "Меньше нагрев и расход батареи",
#ifdef __ANDROID__
        SF_PORT, "Port", "FpsLimit", 0, 0, ST_ENUM, "30", 0, 0, FPSCAP},
#else   /* v1.1 desktop: no cap of the port (the engine itself smooths to 62, bSmoothFrameRate) */
        SF_PORT, "Port", "FpsLimit", 0, 0, ST_ENUM, "0", 0, 0, FPSCAP},
#endif
    {"port.show_fps", "Android", "Показывать FPS", "",
        SF_PORT, "Port", "ShowFps", 0, 0, ST_BOOL01, "0", 0, 0, 0},
    {"port.touch_controls", "Android", "Сенсорное управление", "Экранные кнопки и стики",
        SF_PORT, "Port", "TouchControls", 0, 0, ST_BOOL01, "1", 0, 0, 0},
    {"port.touch_opacity", "Android", "Прозрачность кнопок, %", "",
        SF_PORT, "Port", "TouchOpacity", 0, 0, ST_INT, "60", 10, 100, 0},
    {"port.phone_mod", "Android", "Модификаторы PhoneModify", "Применять модификаторы из папки PhoneModify в папке игры "
        "(файлы .txt): лимиты вызовов отрисовки и примитивов на кадр (maxdraws/maxprims), подмена ключей движка "
        "(ini <секция> <ключ> <значение>), запись байтов в память игры (patch <адрес> <байты>). Заготовку создаёт "
        "генератор: me1cfg --game ... modgen balanced|aggressive|off. Осторожно: лимиты и патчи меняют картинку",
        SF_PORT, "Port", "PhoneMod", 0, 0, ST_BOOL01, "0", 0, 0, 0},
};
const int me1_nsettings = sizeof me1_settings / sizeof me1_settings[0];

static const char* const P_LOW[] = { "screen.resolution=960x540", "screen.vsync=1", "gfx.texture_detail=0", "gfx.filter=0",
    "gfx.dynamic_shadows=0", "gfx.particles=2", "gfx.post_process=0", "gfx.max_texture_size=1024", "snd.channels=32",
    "game.motion_blur=0", "game.film_grain=0", "port.fps_limit=30", "gfx.screen_percentage=100", 0 };
static const char* const P_MED[] = { "screen.resolution=1280x720", "gfx.texture_detail=1", "gfx.filter=1", "gfx.dynamic_shadows=0",
    "gfx.particles=1", "gfx.post_process=1", "gfx.max_texture_size=2048", "snd.channels=48", "game.motion_blur=1", "game.film_grain=0",
    "port.fps_limit=30", "port.simple_shaders=0", "gfx.screen_percentage=100", 0 };
static const char* const P_HIGH[] = { "screen.resolution=1920x1080", "gfx.texture_detail=2", "gfx.filter=2", "gfx.dynamic_shadows=1",
    "gfx.particles=0", "gfx.post_process=1", "gfx.max_texture_size=2048", "snd.channels=64", "game.motion_blur=1", "game.film_grain=1",
    "port.fps_limit=60", "port.simple_shaders=0", "gfx.screen_percentage=100", 0 };
static const char* const P_MIN[] = { "screen.resolution=640x360", "screen.vsync=1", "gfx.texture_detail=0", "gfx.filter=0",
    "gfx.dynamic_shadows=0", "gfx.particles=2", "gfx.post_process=0", "gfx.max_texture_size=1024", "snd.channels=24",
    "game.motion_blur=0", "game.film_grain=0", "port.fps_limit=30", "port.simple_shaders=1", "gfx.screen_percentage=100", 0 };
static const char* const P_ULTRA[] = { "screen.resolution=1920x1080", "screen.vsync=1", "gfx.texture_detail=2", "gfx.filter=2",
    "gfx.dynamic_shadows=1", "gfx.particles=0", "gfx.post_process=1", "gfx.max_texture_size=4096", "snd.channels=64",
    "game.motion_blur=1", "game.film_grain=1", "port.fps_limit=60", "port.simple_shaders=0", "gfx.screen_percentage=100", 0 };
const SettingPreset me1_presets[] = {
    {"minimal", "Минимальные", "Очень слабые GPU (PowerVR GE8xxx, Adreno 50x, Mali-G31/G51): 640×360, упрощённый расчёт", P_MIN},
    {"low", "Низкие", "Слабые GPU (Mali-G52/G57, Adreno 610–619): 960×540, без теней и постобработки, 30 FPS", P_LOW},
    {"medium", "Средние", "Snapdragon 7xx / Dimensity 900 и выше", P_MED},
    {"high", "Высокие", "Флагманы", P_HIGH},
    {"ultra", "Максимальные", "Snapdragon 8 Gen 2/3/Elite, Dimensity 9200+: 1080p, тени, 60 FPS", P_ULTRA},
};
const int me1_npresets = sizeof me1_presets / sizeof me1_presets[0];

/* ---------------- paths ---------------- */
static void join(char* out, size_t cap, const char* a, const char* b) { snprintf(out, cap, "%s%s%s", a, (*a && a[strlen(a) - 1] != '/') ? "/" : "", b); }
static void cfg_dir(const char* docs, char* out, size_t cap) { join(out, cap, docs, "BioWare/Mass Effect/Config"); }
static void overrides_path(const char* docs, char* out, size_t cap) { char d[600]; cfg_dir(docs, d, sizeof d); join(out, cap, d, "me1port_overrides.ini"); }
static void port_path(const char* docs, char* out, size_t cap) { join(out, cap, docs, "BioWare/Mass Effect/me1port.ini"); }
void me1cfg_paths(const char* game, const char* docs, int file, char* user_ini, char* default_ini, size_t cap) {
    char d[600], n[64];
    if (file == SF_PORT) { port_path(docs, user_ini, cap); if (default_ini) default_ini[0] = 0; return; }
    cfg_dir(docs, d, sizeof d); snprintf(n, sizeof n, "BIO%s.ini", FILE_NAME[file]); join(user_ini, cap, d, n);
    if (default_ini) { snprintf(n, sizeof n, "BioGame/Config/Default%s.ini", FILE_NAME[file]); join(default_ini, cap, game, n); }
}
static int mkdirs(const char* path) {   /* mkdir -p for the directory part of path (or path itself if it ends in '/') */
    char p[1024]; snprintf(p, sizeof p, "%s", path);
    for (char* s = p + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(p, 0775) && errno != EEXIST) return -1; *s = '/'; }
    return 0;
}


/* ---------------- model ---------------- */
const SettingDef* me1cfg_find(const char* id, int* index) {
    for (int i = 0; i < me1_nsettings; i++) if (!strcmp(me1_settings[i].id, id)) { if (index) *index = i; return &me1_settings[i]; }
    return NULL;
}
/* ini text -> canonical value */
static int from_ini(const SettingDef* d, const Ini* ini, const char* sec, char* out, size_t cap) {
    char a[256], b[256];
    if (!ini_get(ini, sec, d->key, a, sizeof a)) return 0;
    switch (d->type) {
    case ST_BOOL: case ST_BOOL01: snprintf(out, cap, "%d", ini_parse_bool(a, atoi(d->def))); return 1;
    case ST_INT: snprintf(out, cap, "%d", atoi(a)); return 1;
    case ST_FLOAT: snprintf(out, cap, "%g", atof(a)); return 1;
    case ST_RES: if (!ini_get(ini, sec, d->key2, b, sizeof b)) return 0; snprintf(out, cap, "%dx%d", atoi(a), atoi(b)); return 1;
    case ST_ENUM: {
        size_t l = strlen(a); while (l && (a[l - 1] == ';' || a[l - 1] == ' ')) a[--l] = 0;
        if (l >= 2 && a[0] == '"' && a[l - 1] == '"') { memmove(a, a + 1, l - 2); a[l - 2] = 0; }
        for (const SetChoice* ch = d->choices; ch && ch->value; ch++) if (!strcasecmp(ch->value, a)) { snprintf(out, cap, "%s", ch->value); return 1; }
        snprintf(out, cap, "%s", a); return 1; }
    default: snprintf(out, cap, "%s", a); return 1;
    }
}
static void to_ini(const SettingDef* d, Ini* ini, const char* sec, const char* v) {
    char a[64], b[64];
    switch (d->type) {
    case ST_BOOL: ini_set(ini, sec, d->key, atoi(v) ? "True" : "False"); break;
    case ST_BOOL01: ini_set(ini, sec, d->key, atoi(v) ? "1" : "0"); break;
    case ST_RES: { int w = 0, h = 0; sscanf(v, "%dx%d", &w, &h); snprintf(a, sizeof a, "%d", w); snprintf(b, sizeof b, "%d", h);
        ini_set(ini, sec, d->key, a); ini_set(ini, sec, d->key2, b); break; }
    case ST_INT: ini_set(ini, sec, d->key, v);
        if (d->key2) { snprintf(a, sizeof a, "%d", atoi(v) - 2); ini_set(ini, sec, d->key2, a); }   /* snd.channels: ISACT keeps 2 reserved */
        break;
    default: ini_set(ini, sec, d->key, v);
    }
}
static int validate(const SettingDef* d, const char* v, char* norm, size_t cap, char* err, size_t errcap) {
    char* end;
    switch (d->type) {
    case ST_BOOL: case ST_BOOL01: {
        int b = ini_parse_bool(v, -1); if (b < 0) { snprintf(err, errcap, "%s: ожидается 0/1", d->id); return -1; }
        snprintf(norm, cap, "%d", b); return 0; }
    case ST_INT: { long x = strtol(v, &end, 10); if (*end || end == v) { snprintf(err, errcap, "%s: ожидается целое", d->id); return -1; }
        if (d->max > d->min && (x < d->min || x > d->max)) { snprintf(err, errcap, "%s: допустимо %g..%g", d->id, d->min, d->max); return -1; }
        snprintf(norm, cap, "%ld", x); return 0; }
    case ST_FLOAT: { double x = strtod(v, &end); if (*end || end == v) { snprintf(err, errcap, "%s: ожидается число", d->id); return -1; }
        if (d->max > d->min && (x < d->min || x > d->max)) { snprintf(err, errcap, "%s: допустимо %g..%g", d->id, d->min, d->max); return -1; }
        snprintf(norm, cap, "%g", x); return 0; }
    case ST_RES: { int w, h; char tail; if (sscanf(v, "%dx%d%c", &w, &h, &tail) != 2 || w < 320 || h < 200 || w > 7680 || h > 4320) {
            snprintf(err, errcap, "%s: формат ШИРИНАxВЫСОТА, от 320x200", d->id); return -1; }
        snprintf(norm, cap, "%dx%d", w, h); return 0; }
    case ST_ENUM:
        for (const SetChoice* ch = d->choices; ch && ch->value; ch++) if (!strcasecmp(ch->value, v)) { snprintf(norm, cap, "%s", ch->value); return 0; }
        snprintf(err, errcap, "%s: недопустимое значение '%s'", d->id, v); return -1;
    default: snprintf(norm, cap, "%s", v); return 0;
    }
}
static void ov_section(int file, const char* sec, char* out, size_t cap) { snprintf(out, cap, "%s|%s", FILE_NAME[file], sec); }

int me1cfg_load(ME1Config* c, const char* game, const char* docs) {
    memset(c, 0, sizeof *c);
    snprintf(c->game_dir, sizeof c->game_dir, "%s", game); snprintf(c->docs_dir, sizeof c->docs_dir, "%s", docs);
    Ini user[SF_COUNT], def[SF_COUNT], ov; int have_user[SF_COUNT];
    char up[1024], dp[1024], sec[300];
    for (int f = 0; f < SF_COUNT; f++) {
        me1cfg_paths(game, docs, f, up, dp, sizeof up);
        have_user[f] = ini_load_file(&user[f], up) == 0;
        if (f != SF_PORT) ini_load_file(&def[f], dp); else ini_init(&def[f]);
    }
    overrides_path(docs, up, sizeof up); ini_load_file(&ov, up);
    for (int i = 0; i < me1_nsettings; i++) {
        const SettingDef* d = &me1_settings[i]; char* v = c->value[i];
        ov_section(d->file, d->section, sec, sizeof sec);
        if (have_user[d->file] && from_ini(d, &user[d->file], d->section, v, 96)) strcpy(c->source[i], "user");
        else if (from_ini(d, &ov, sec, v, 96)) strcpy(c->source[i], "override");
        else if (from_ini(d, &def[d->file], d->section, v, 96)) strcpy(c->source[i], "default");
        else { snprintf(v, 96, "%s", d->def); strcpy(c->source[i], "builtin"); }
    }
    for (int f = 0; f < SF_COUNT; f++) { ini_free(&user[f]); ini_free(&def[f]); }
    ini_free(&ov);
    return 0;
}
int me1cfg_set(ME1Config* c, const char* id, const char* value, char* err, size_t errcap) {
    int i; const SettingDef* d = me1cfg_find(id, &i);
    if (!d) { snprintf(err, errcap, "неизвестная настройка '%s'", id); return -1; }
    char norm[96]; if (validate(d, value, norm, sizeof norm, err, errcap)) return -1;
    if (strcmp(c->value[i], norm)) { strcpy(c->value[i], norm); c->dirty[i] = 1; }
    return 0;
}
int me1cfg_preset(ME1Config* c, const char* preset, char* err, size_t errcap) {
    for (int p = 0; p < me1_npresets; p++) if (!strcmp(me1_presets[p].id, preset)) {
        for (const char* const* kv = me1_presets[p].kv; *kv; kv++) {
            char id[64]; const char* eq = strchr(*kv, '='); snprintf(id, sizeof id, "%.*s", (int)(eq - *kv), *kv);
            if (me1cfg_set(c, id, eq + 1, err, errcap)) return -1;
        }
        return 0;
    }
    snprintf(err, errcap, "неизвестный пресет '%s'", preset); return -1;
}
/* ---------------- automatic preset (v0.9.7): GPU / SoC / RAM / CPU / screen -> tier ---------------- */
static const char* const TIER_NAME[5] = { "минимальный", "низкий", "средний", "высокий", "максимальный" };
static const char* const TIER_PRESET[5] = { "minimal", "low", "medium", "high", "ultra" };
static const char* ci_find(const char* h, const char* n) { size_t l = strlen(n); for (; h && *h; h++) if (!strncasecmp(h, n, l)) return h; return NULL; }
static int num_after(const char* p) { while (*p && !(*p >= '0' && *p <= '9')) p++; return atoi(p); }
static int gpu_tier(const char* g, char* why, size_t cap) {
    const char* p;
    if (!g || !*g) return -1;
    if ((p = ci_find(g, "Adreno"))) {
        int n = num_after(p + 6);
        int t = n >= 800 ? 4 : n >= 740 ? 4 : n >= 725 ? 3 : n >= 700 ? 2 : n >= 650 ? 3 : n >= 630 ? 2 : n >= 610 ? 1 : n >= 530 ? 1 : 0;
        snprintf(why, cap, "Adreno %d", n); return t;
    }
    if ((p = ci_find(g, "Immortalis"))) { snprintf(why, cap, "%s", g); return 4; }
    if ((p = ci_find(g, "Mali"))) {
        const char* q = p + 4; while (*q == '-' || *q == ' ') q++;
        if (*q == 'T' || *q == 't') { snprintf(why, cap, "%s (Midgard)", g); return 0; }
        int model = num_after(q), mc = 1; const char* m = ci_find(q, "MC"); if (!m) m = ci_find(q, "MP");
        if (m) { mc = atoi(m + 2); if (mc < 1) mc = 1; }
        static const struct { int model; double per_core; } S[] = {
            {31, 0.4}, {51, 0.6}, {52, 1.0}, {57, 1.3}, {68, 1.5}, {71, 0.5}, {72, 0.6}, {76, 1.5}, {77, 2.0}, {78, 2.0},
            {310, 0.8}, {510, 1.2}, {610, 2.0}, {615, 2.4}, {620, 2.6}, {710, 2.4}, {715, 2.8}, {720, 3.0}, {725, 3.4}, {925, 3.6} };
        double pc = model >= 100 ? 2.0 : 0.8;
        for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) if (S[i].model == model) pc = S[i].per_core;
        double sc = pc * mc;
        snprintf(why, cap, "Mali-G%d MC%d", model, mc);
        return sc < 1.8 ? 0 : sc < 3.5 ? 1 : sc < 11 ? 2 : sc < 30 ? 3 : 4;
    }
    if ((p = ci_find(g, "Xclipse"))) { int n = num_after(p + 7); snprintf(why, cap, "Xclipse %d", n); return n >= 940 ? 4 : n >= 900 ? 3 : 2; }
    if (ci_find(g, "Maleoon")) { snprintf(why, cap, "%s", g); return 3; }
    if (ci_find(g, "PowerVR") || ci_find(g, "IMG ")) {
        snprintf(why, cap, "%s", g);
        return ci_find(g, "GE8") || ci_find(g, "G6") || ci_find(g, "GX6") ? 0 : ci_find(g, "BXM") ? 1 : 2;
    }
    if (ci_find(g, "Tegra")) { snprintf(why, cap, "%s", g); return 2; }
    return -1;
}
static int soc_tier(const char* s, char* why, size_t cap) {   /* fallback when GL_RENDERER is unknown */
    if (!s || !*s) return -1;
    const char* p;
    if (ci_find(s, "mt6769") || ci_find(s, "mt6768") || ci_find(s, "mt6785") || ci_find(s, "mt6781")) { snprintf(why, cap, "%s (Helio G8x/G9x)", s); return 1; }
    if ((p = ci_find(s, "sm8"))) { int n = num_after(p); snprintf(why, cap, "Snapdragon SM%d", n); return n >= 8550 ? 4 : n >= 8350 ? 3 : 2; }
    if ((p = ci_find(s, "sm7"))) { snprintf(why, cap, "Snapdragon SM%d", num_after(p)); return 2; }
    if ((p = ci_find(s, "sm6"))) { snprintf(why, cap, "Snapdragon SM%d", num_after(p)); return 1; }
    if ((p = ci_find(s, "sm4")) || ci_find(s, "mt676") || ci_find(s, "mt6765")) { snprintf(why, cap, "%s", s); return 0; }
    static const struct { const char* name; int tier; } C[] = { {"pineapple", 4}, {"kalama", 4}, {"sun", 4}, {"taro", 3}, {"lahaina", 3},
        {"kona", 3}, {"msmnile", 2}, {"sdm845", 2}, {"lito", 2}, {"atoll", 2}, {"trinket", 1}, {"bengal", 1}, {"holi", 1}, {"sdm660", 1} };
    for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++) if (!strcasecmp(s, C[i].name)) { snprintf(why, cap, "%s", s); return C[i].tier; }
    return -1;
}
int me1cfg_classify(const ME1Device* d, char* report, size_t cap, int* fps, char* res, size_t rescap) {
    char why[128] = "", w2[128] = ""; size_t k = 0;
    int t = gpu_tier(d->gpu, why, sizeof why), from_gpu = t >= 0;
    if (t < 0) t = soc_tier(d->soc, why, sizeof why);
    if (t < 0) { t = 1; snprintf(why, sizeof why, "GPU не распознан (%s)", d->gpu && *d->gpu ? d->gpu : d->soc && *d->soc ? d->soc : "?"); }
    k += (size_t)snprintf(report + k, cap - k, "%s%s → класс «%s»", from_gpu ? "GPU " : "", why, TIER_NAME[t]);
    int cap_t = 4;
    if (d->ram_mb > 0) {
        cap_t = d->ram_mb < 3000 ? 0 : d->ram_mb < 4500 ? 1 : d->ram_mb < 6000 ? 2 : 4;
        if (cap_t < t) { t = cap_t; snprintf(w2, sizeof w2, "; мало ОЗУ → «%s»", TIER_NAME[t]); k += (size_t)snprintf(report + k, cap - k, "%s", w2); }
    }
    if (d->max_mhz > 0 && d->max_mhz < 1800 && t > 1) { t = 1; k += (size_t)snprintf(report + k, cap - k, "; CPU до %.1f ГГц → «%s»", d->max_mhz / 1000.0, TIER_NAME[t]); }
    /* frame cap: the recompiled x86 code is CPU-bound - 60 only for the fastest cores */
    *fps = t >= 4 && d->max_mhz >= 3000 ? 60 : t >= 3 && d->max_mhz >= 2800 ? 45 : 30;
    if (t >= 4 && d->max_mhz > 0 && d->max_mhz < 3000) *fps = 45;
    /* 16:9 render resolution: the tier's choice, capped by the screen */
    static const int RW[5] = { 640, 960, 1280, 1600, 1920 }, RH[5] = { 360, 540, 720, 900, 1080 };
    static const int AW[7] = { 640, 854, 960, 1024, 1280, 1600, 1920 }, AH[7] = { 360, 480, 540, 576, 720, 900, 1080 };
    int rw = RW[t], rh = RH[t];
    int sw = d->screen_w > d->screen_h ? d->screen_w : d->screen_h, sh = d->screen_w > d->screen_h ? d->screen_h : d->screen_w;
    if (sh > 0 && (rh > sh || rw > sw)) {
        rw = AW[0]; rh = AH[0];
        for (int i = 0; i < 7; i++) if (AH[i] <= sh && AW[i] <= sw) { rw = AW[i]; rh = AH[i]; }
    }
    snprintf(res, rescap, "%dx%d", rw, rh);
    k += (size_t)snprintf(report + k, cap - k, ". ");
    if (d->ram_mb > 0) k += (size_t)snprintf(report + k, cap - k, "ОЗУ %.1f ГБ, ", d->ram_mb / 1024.0);
    if (d->cores > 0) k += (size_t)snprintf(report + k, cap - k, "CPU %d ядер%s", d->cores, d->max_mhz > 0 ? "" : ", ");
    if (d->max_mhz > 0) k += (size_t)snprintf(report + k, cap - k, "%sдо %.1f ГГц, ", d->cores > 0 ? " " : "CPU ", d->max_mhz / 1000.0);
    if (sw > 0) k += (size_t)snprintf(report + k, cap - k, "экран %d×%d, ", sw, sh);
    snprintf(report + k, cap - k, "рендер %d×%d, %d FPS", rw, rh, *fps);
    return t;
}
int me1cfg_auto(ME1Config* c, const ME1Device* d, char* report, size_t cap) {
    char res[32], err[256]; int fps;
    int t = me1cfg_classify(d, report, cap, &fps, res, sizeof res);
    char fs[8]; snprintf(fs, sizeof fs, "%d", fps);
    if (me1cfg_preset(c, TIER_PRESET[t], err, sizeof err) || me1cfg_set(c, "screen.resolution", res, err, sizeof err) ||
        me1cfg_set(c, "port.fps_limit", fs, err, sizeof err)) { snprintf(report, cap, "ошибка: %s", err); return -1; }
    return t;
}
int me1cfg_reset(ME1Config* c) {
    Ini def[SF_COUNT]; char up[1024], dp[1024];
    for (int f = 0; f < SF_COUNT; f++) { me1cfg_paths(c->game_dir, c->docs_dir, f, up, dp, sizeof up); if (f != SF_PORT) ini_load_file(&def[f], dp); else ini_init(&def[f]); }
    for (int i = 0; i < me1_nsettings; i++) {
        const SettingDef* d = &me1_settings[i]; char v[96];
        if (!from_ini(d, &def[d->file], d->section, v, sizeof v)) snprintf(v, sizeof v, "%s", d->def);
        strcpy(c->value[i], v); c->dirty[i] = 1; strcpy(c->source[i], "reset");
    }
    for (int f = 0; f < SF_COUNT; f++) ini_free(&def[f]);
    return 0;
}
int me1cfg_save(ME1Config* c, char* msg, size_t cap) {
    char up[1024], dp[1024], sec[300]; size_t used = 0; msg[0] = 0;
    int reset = 0; for (int i = 0; i < me1_nsettings; i++) if (!strcmp(c->source[i], "reset")) reset = 1;
    /* overrides: everything that is not at its Default*.ini value, or was explicitly chosen */
    Ini ov; overrides_path(c->docs_dir, up, sizeof up);
    if (mkdirs(up)) { snprintf(msg, cap, "не удалось создать папку для %s", up); return -1; }
    if (reset) ini_init(&ov); else ini_load_file(&ov, up);
    if (!ov.n) { const char* hdr = "; Mass Effect port (me1port): overrides applied to Default*.ini when the game reads them"; ini_load_mem(&ov, hdr, strlen(hdr)); }
    for (int i = 0; i < me1_nsettings; i++) {
        const SettingDef* d = &me1_settings[i]; if (d->file == SF_PORT) continue;
        if (!c->dirty[i] || reset) continue;
        ov_section(d->file, d->section, sec, sizeof sec); to_ini(d, &ov, sec, c->value[i]);
        if (d->section2) { ov_section(d->file, d->section2, sec, sizeof sec); to_ini(d, &ov, sec, c->value[i]); }
    }
    if (ini_save_file(&ov, up)) { ini_free(&ov); snprintf(msg, cap, "ошибка записи %s", up); return -1; }
    ini_free(&ov);
    used += snprintf(msg + used, cap - used, "переопределения: %s\n", up);
    /* user files written by the game: update only what changed, keep everything else (incl. [IniVersion]) */
    for (int f = 0; f < SF_COUNT; f++) {
        int any = 0; for (int i = 0; i < me1_nsettings; i++) if (me1_settings[i].file == f && c->dirty[i]) any = 1;
        if (!any && f != SF_PORT) continue;
        me1cfg_paths(c->game_dir, c->docs_dir, f, up, dp, sizeof up);
        Ini u;
        if (ini_load_file(&u, up) != 0) {
            if (f != SF_PORT) { if (used < cap) used += snprintf(msg + used, cap - used, "BIO%s.ini ещё не создан игрой — значения войдут в него при первом запуске\n", FILE_NAME[f]); continue; }
            if (mkdirs(up)) { snprintf(msg, cap, "не удалось создать папку для %s", up); return -1; }
            const char* hdr = "; Mass Effect Android port options"; ini_load_mem(&u, hdr, strlen(hdr));
        }
        for (int i = 0; i < me1_nsettings; i++) {
            const SettingDef* d = &me1_settings[i]; if (d->file != f || (!c->dirty[i] && f != SF_PORT)) continue;
            to_ini(d, &u, d->section, c->value[i]);
            if (d->section2 && ini_has_section(&u, d->section2)) to_ini(d, &u, d->section2, c->value[i]);
        }
        int rc = ini_save_file(&u, up); ini_free(&u);
        if (rc) { snprintf(msg, cap, "ошибка записи %s", up); return -1; }
        if (used < cap) used += snprintf(msg + used, cap - used, "обновлён: %s\n", up);
    }
    for (int i = 0; i < me1_nsettings; i++) { c->dirty[i] = 0; if (!strcmp(c->source[i], "reset")) strcpy(c->source[i], "default"); }
    return 0;
}
char* me1cfg_dump(const ME1Config* c) {
    size_t cap = 256 * (me1_nsettings + 1), k = 0; char* o = malloc(cap);
    for (int i = 0; i < me1_nsettings; i++) k += snprintf(o + k, cap - k, "%s=%s\t%s\n", me1_settings[i].id, c->value[i], c->source[i]);
    return o;
}
static size_t jstr(char* o, size_t k, size_t cap, const char* s) {
    if (k < cap) o[k++] = '"';
    for (; s && *s && k + 8 < cap; s++) {
        if (*s == '"' || *s == '\\') { o[k++] = '\\'; o[k++] = *s; }
        else if ((unsigned char)*s < 0x20) k += snprintf(o + k, cap - k, "\\u%04x", *s);
        else o[k++] = *s;
    }
    if (k < cap) o[k++] = '"';
    return k;
}
char* me1cfg_schema_json(void) {
    static const char* const TN[] = { "bool", "bool", "int", "float", "enum", "resolution", "string" };
    size_t cap = 65536, k = 0; char* o = malloc(cap);
    k += snprintf(o + k, cap - k, "{\"settings\":[");
    for (int i = 0; i < me1_nsettings; i++) {
        const SettingDef* d = &me1_settings[i];
        k += snprintf(o + k, cap - k, "%s{\"id\":", i ? "," : ""); k = jstr(o, k, cap, d->id);
        k += snprintf(o + k, cap - k, ",\"group\":"); k = jstr(o, k, cap, d->group);
        k += snprintf(o + k, cap - k, ",\"label\":"); k = jstr(o, k, cap, d->label);
        k += snprintf(o + k, cap - k, ",\"help\":"); k = jstr(o, k, cap, d->help);
        k += snprintf(o + k, cap - k, ",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"choices\":[", TN[d->type], d->min, d->max);
        int first = 1;
        for (const SetChoice* ch = d->choices; ch && ch->value; ch++) {
            k += snprintf(o + k, cap - k, "%s[", first ? "" : ","); k = jstr(o, k, cap, ch->value);
            k += snprintf(o + k, cap - k, ","); k = jstr(o, k, cap, ch->label); k += snprintf(o + k, cap - k, "]"); first = 0;
        }
        k += snprintf(o + k, cap - k, "]}");
    }
    k += snprintf(o + k, cap - k, "],\"presets\":[");
    for (int p = 0; p < me1_npresets; p++) {
        k += snprintf(o + k, cap - k, "%s{\"id\":", p ? "," : ""); k = jstr(o, k, cap, me1_presets[p].id);
        k += snprintf(o + k, cap - k, ",\"label\":"); k = jstr(o, k, cap, me1_presets[p].label);
        k += snprintf(o + k, cap - k, ",\"help\":"); k = jstr(o, k, cap, me1_presets[p].help); k += snprintf(o + k, cap - k, "}");
    }
    k += snprintf(o + k, cap - k, "]}");
    return o;
}
typedef struct { char secs[64][256]; int ns; const char* prefix; size_t pl; } PatchSecs;
typedef struct { Ini* d; const char* sec; int changed; } PatchKeys;
static void patch_collect(const char* s, void* u) {
    PatchSecs* p = u;
    if (p->ns < 64 && !strncasecmp(s, p->prefix, p->pl)) snprintf(p->secs[p->ns++], 256, "%s", s);
}
static void patch_apply(const char* k, const char* v, void* u) {
    PatchKeys* p = u; char cur[512], val[512];
    size_t l = strlen(v); while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
    snprintf(val, sizeof val, "%.*s", (int)l, v);
    if (!ini_get(p->d, p->sec, k, cur, sizeof cur) || strcmp(cur, val)) { ini_set(p->d, p->sec, k, val); p->changed = 1; }
}
int me1cfg_patch_default(const char* docs, const char* name, const void* in, size_t n, void** out, size_t* outn) {
    char p[1024], prefix[64]; Ini ov, d;
    overrides_path(docs, p, sizeof p);
    if (ini_load_file(&ov, p) != 0) return 0;
    snprintf(prefix, sizeof prefix, "%s|", name);
    PatchSecs* ps = calloc(1, sizeof *ps); ps->prefix = prefix; ps->pl = strlen(prefix);
    ini_sections(&ov, patch_collect, ps);
    ini_load_mem(&d, in, n);
    PatchKeys pk = { &d, NULL, 0 };
    for (int i = 0; i < ps->ns; i++) { pk.sec = ps->secs[i] + ps->pl; ini_keys(&ov, ps->secs[i], patch_apply, &pk); }
    if (pk.changed) *out = ini_serialize(&d, outn);
    free(ps); ini_free(&ov); ini_free(&d);
    return pk.changed;
}
int me1cfg_port_get(const char* docs, const char* id, char* out, size_t cap) {
    const SettingDef* d = me1cfg_find(id, NULL); if (!d || d->file != SF_PORT) return 0;
    char p[1024]; Ini u; port_path(docs, p, sizeof p);
    int ok = ini_load_file(&u, p) == 0 && from_ini(d, &u, d->section, out, cap);
    if (!ok) snprintf(out, cap, "%s", d->def);
    ini_free(&u); return 1;
}
