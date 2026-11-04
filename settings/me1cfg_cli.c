/* me1cfg: command-line replacement for MassEffectConfig.exe (host / adb shell / tests).
 *   me1cfg [--game DIR] [--docs DIR] show | get ID | set ID=VALUE... | preset NAME | auto GPU [SOC RAM CORES MHZ WxH] | presets | reset | schema | patch Engine|Game|Input | paths
 * Defaults: --game $ME1_GAME or ".", --docs $ME1_DOCS or "<game>/Documents". */
#include "me1settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static void usage(void) {
    fprintf(stderr,
        "me1cfg [--game DIR] [--docs DIR] команда\n"
        "  show                 все настройки (значение, источник)\n"
        "  get ID               одно значение\n"
        "  set ID=ЗНАЧ ...      изменить и сохранить\n"
        "  preset ИМЯ           применить пресет (minimal, low, medium, high, ultra)\n"
        "  auto GPU [SOC ОЗУ_МБ ЯДЕР МГЦ ШxВ]   автопресет по устройству (как кнопка «Авто»)\n"
        "  presets              список пресетов\n"
        "  reset                вернуть значения из Default*.ini\n"
        "  schema               JSON-схема для интерфейса\n"
        "  patch Engine|Game|Input   показать Default<Имя>.ini так, как его увидит игра\n"
        "  paths                пути к файлам\n"
        "  modgen light|balanced|aggressive|extreme|cpu   создать PhoneModify/phone_tune.txt (cpu — для устройств, где GPU не загружен)\n"
        "  modgen auto GPU                     подобрать уровень по GPU (Mali-G52 -> aggressive)\n"
        "  modgen custom draws=N prims=N ...   свои лимиты (draws prims particles screen shadows decals detail skeletal aniso simplefx directional)\n"
        "  modgen off                          удалить phone_tune.txt\n");
}
static void pad(const char* s, int w) {   /* left-aligned, width in characters (UTF-8 aware) */
    int n = 0; for (const unsigned char* p = (const unsigned char*)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    fputs(s, stdout); for (; n < w; n++) putchar(' ');
}
static void show(const ME1Config* c) {
    const char* grp = "";
    for (int i = 0; i < me1_nsettings; i++) {
        const SettingDef* d = &me1_settings[i];
        if (strcmp(grp, d->group)) { printf("%s[%s]\n", i ? "\n" : "", d->group); grp = d->group; }
        const char* lab = c->value[i];
        for (const SetChoice* ch = d->choices; ch && ch->value; ch++) if (!strcmp(ch->value, c->value[i])) lab = ch->label;
        if (d->type == ST_BOOL || d->type == ST_BOOL01) lab = atoi(c->value[i]) ? "вкл" : "выкл";
        printf("  "); pad(d->label, 28); pad(c->value[i], 18); pad(lab, 30);
        printf("%-9s %s\n", c->source[i], d->id);
    }
}
static int save(ME1Config* c) {
    char msg[4096];
    if (me1cfg_save(c, msg, sizeof msg)) { fprintf(stderr, "ошибка: %s\n", msg); return 1; }
    fputs(msg, stdout); return 0;
}
/* v0.10.4: PhoneModify generator - writes <game>/PhoneModify/phone_tune.txt (see runtime/hle_mod.c).
 *   modgen light|balanced|aggressive|extreme          готовый уровень
 *   modgen auto "GPU"                                 подобрать уровень по GPU
 *   modgen custom КЛЮЧ=ЗНАЧ ...                       свои значения
 *   modgen off                                        удалить phone_tune.txt
 * Ключи INI - те, что читает FSystemSettings::LoadFromIni (0x10e74030) из [SystemSettings] BIOEngine.ini. */
typedef struct ModTune {
    int particles;        /* ParticleLODBias: 0 полно, 1/2 беднее (0 - не писать) */
    int shadows;          /* DynamicShadows=False + LightEnvironmentShadows=False */
    int decals;           /* DynamicDecals=False */
    int simplefx;         /* MotionBlur/DepthOfField/LensFlares/QualityBloom=False */
    int directional;      /* DirectionalLightmaps=False */
    int detailmode;       /* DetailMode (0 не пишет) */
    int skeletal;         /* SkeletalMeshLODBias (0 не пишет) */
    int aniso;            /* MaxAnisotropy<=N (0 не пишет) */
    int screen;           /* ScreenPercentage % (100 не пишет) */
    unsigned maxdraws;    /* 0 не пишет */
    unsigned maxprims;    /* 0 не пишет */
} ModTune;
static const ModTune TUNE_LIGHT      = { 0, 1, 0, 0, 0, 0, 0, 0, 100,       0,       0 };
static const ModTune TUNE_BALANCED   = { 1, 1, 1, 0, 0, 0, 1, 0, 100,    6000, 3000000 };
static const ModTune TUNE_AGGRESSIVE = { 2, 1, 1, 1, 0, 1, 1, 0,  70,    3500, 1800000 };
static const ModTune TUNE_EXTREME    = { 2, 1, 1, 1, 1, 1, 2, 1,  60,    2500, 1200000 };
static const ModTune TUNE_CPU        = { 2, 1, 1, 1, 0, 1, 1, 0, 100,    1200,  300000 };   /* CPU-bound devices: GPU has headroom, cut draw calls */
static int modgen_write(const char* game, const ModTune* t, const char* title) {
    char dir[1024], path[2048];
    snprintf(dir, sizeof dir, "%s/PhoneModify", game);
#ifdef _WIN32
    _mkdir(dir);
#else
    if (mkdir(dir, 0777) && errno != EEXIST) { fprintf(stderr, "ошибка: cannot create %s\n", dir); return 1; }
#endif
    snprintf(path, sizeof path, "%s/phone_tune.txt", dir);
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "ошибка: cannot write %s\n", path); return 1; }
    fprintf(f,
        "# PhoneModify: %s\n"
        "# Включить: переключатель \"Модификаторы PhoneModify\" в лаунчере (или ME1_PHONE_MOD=1).\n"
        "# Проверка в логе: [mod] PhoneModify active: ... и ', capped N draws' в строке кадра.\n"
        "# Директивы: maxdraws/maxprims - лимиты на кадр; ini <секция> <ключ> <значение> - подмена\n"
        "# ключа движка в памяти (диск не меняется); patch <адрес> <байты> - запись в память игры.\n\n", title);
    if (t->particles) fprintf(f, "ini SystemSettings ParticleLODBias %d          # частицы беднее: снегопад PeakDrift и прочий жор\n", t->particles);
    if (t->shadows) fprintf(f, "ini SystemSettings DynamicShadows False\nini SystemSettings LightEnvironmentShadows False    # проходы теней\n");
    if (t->decals) fprintf(f, "ini SystemSettings DynamicDecals False        # кровь/следы: каждая перерисовывает геометрию\n");
    if (t->simplefx) fprintf(f, "ini SystemSettings MotionBlur False\nini SystemSettings DepthOfField False\nini SystemSettings LensFlares False\nini SystemSettings QualityBloom False\n");
    if (t->directional) fprintf(f, "ini SystemSettings DirectionalLightmaps False   # 1 карта освещения вместо 3\n");
    if (t->detailmode) fprintf(f, "ini SystemSettings DetailMode %d              # мелкие детали окружения\n", t->detailmode);
    if (t->skeletal) fprintf(f, "ini SystemSettings SkeletalMeshLODBias %d            # персонажи на LOD ниже\n", t->skeletal);
    if (t->aniso) fprintf(f, "ini SystemSettings MaxAnisotropy %d                 # анизотропия (на Mali почти бесплатно и так)\n", t->aniso);
    if (t->screen && t->screen != 100) fprintf(f, "ini SystemSettings ScreenPercentage %d            # сцена в %d%% разрешения, интерфейс полный\n", t->screen, t->screen);
    if (t->maxdraws) fprintf(f, "maxdraws %u\n", t->maxdraws);
    if (t->maxprims) fprintf(f, "maxprims %u\n", t->maxprims);
    fprintf(f,
        "\n# Тонкая настройка вручную: раскомментируй и поправь.\n"
        "# maxdraws 2000\n"
        "# maxprims 800000\n"
        "# ini SystemSettings ScreenPercentage 50\n"
        "# patch 10e7dc00 90 90 90        # пример: байты по адресу из лога (m_10e7dc00)\n");
    fclose(f);
    printf("создан %s (%s; включи «Модификаторы PhoneModify» в лаунчере или ME1_PHONE_MOD=1)\n", path, title);
    return 0;
}
static int modgen_level(const char* game, const char* kind) {
    const char* title = kind;
    const ModTune* t;
    if      (!strcmp(kind, "light"))      t = &TUNE_LIGHT;
    else if (!strcmp(kind, "balanced"))   t = &TUNE_BALANCED;
    else if (!strcmp(kind, "aggressive")) t = &TUNE_AGGRESSIVE;
    else if (!strcmp(kind, "extreme"))    t = &TUNE_EXTREME;
    else if (!strcmp(kind, "cpu"))        t = &TUNE_CPU;
    else return 2;
    return modgen_write(game, t, title);
}
static int modgen_auto(const char* game, int nargs, char** args) {
    const char* g = nargs > 0 ? args[0] : "";
    char l[64]; snprintf(l, sizeof l, "%s", g);
    for (char* p = l; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    const char* kind = "balanced";   /* G6x/G7x, Adreno 7xx и новее */
    if (strstr(l, "mali-g31") || strstr(l, "mali-g5") || strstr(l, "mali-g77") || strstr(l, "adreno 6") || strstr(l, "xclipse")) kind = "aggressive";
    if (strstr(l, "powervr") || strstr(l, "adreno 5") || strstr(l, "adreno 3") || strstr(l, "adreno 4") || strstr(l, "mali-g2")) kind = "extreme";
    if (strstr(l, "mali-g61") || strstr(l, "mali-g68") || strstr(l, "mali-g715") || strstr(l, "mali-g720") ||
        strstr(l, "adreno 740") || strstr(l, "adreno 750") || strstr(l, "adreno 8")) kind = "light";
    printf("GPU \"%s\" -> %s\n", g, kind);
    return modgen_level(game, kind);
}
static int modgen_custom(const char* game, int nargs, char** args) {
    ModTune t; memset(&t, 0, sizeof t);
    for (int i = 0; i < nargs; i++) {
        char* eq = strchr(args[i], '=');
        if (!eq) { fprintf(stderr, "custom: ожидался КЛЮЧ=ЗНАЧ, получено \"%s\"\n", args[i]); return 2; }
        *eq = 0; const char* k = args[i]; unsigned v = (unsigned)strtoul(eq + 1, NULL, 10);
        if      (!strcmp(k, "draws"))       t.maxdraws = v;
        else if (!strcmp(k, "prims"))       t.maxprims = v;
        else if (!strcmp(k, "particles"))   t.particles = (int)v;
        else if (!strcmp(k, "screen"))      t.screen = (int)v;
        else if (!strcmp(k, "shadows"))     t.shadows = v == 0;
        else if (!strcmp(k, "decals"))      t.decals = v == 0;
        else if (!strcmp(k, "detail"))      t.detailmode = (int)v;
        else if (!strcmp(k, "skeletal"))    t.skeletal = (int)v;
        else if (!strcmp(k, "aniso"))       t.aniso = (int)v;
        else if (!strcmp(k, "simplefx"))    t.simplefx = v != 0;
        else if (!strcmp(k, "directional")) t.directional = v != 0;
        else { fprintf(stderr, "custom: неизвестный ключ \"%s\" (draws prims particles screen shadows decals detail skeletal aniso simplefx directional)\n", k); return 2; }
    }
    return modgen_write(game, &t, "custom");
}
static int modgen(const char* game, const char* kind, int nargs, char** args) {
    if (!strcmp(kind, "off")) {
        char p[2048]; snprintf(p, sizeof p, "%s/PhoneModify/phone_tune.txt", game); remove(p);
        printf("PhoneModify/phone_tune.txt удалён (остальные файлы не тронуты)\n");
        return 0;
    }
    if (!strcmp(kind, "auto")) return modgen_auto(game, nargs, args);
    if (!strcmp(kind, "custom")) return modgen_custom(game, nargs, args);
    return modgen_level(game, kind);
}
int main(int argc, char** argv) {
    const char* game = getenv("ME1_GAME"); const char* docs = getenv("ME1_DOCS");
    char docsbuf[1024];
    int a = 1;
    for (; a + 1 < argc && !strncmp(argv[a], "--", 2); a += 2) {
        if (!strcmp(argv[a], "--game")) game = argv[a + 1];
        else if (!strcmp(argv[a], "--docs")) docs = argv[a + 1];
        else { usage(); return 2; }
    }
    if (!game) game = ".";
    if (!docs) { snprintf(docsbuf, sizeof docsbuf, "%s/Documents", game); docs = docsbuf; }
    if (a >= argc) { usage(); return 2; }
    const char* cmd = argv[a++];
    ME1Config c; char err[512];
    if (!strcmp(cmd, "schema")) { char* j = me1cfg_schema_json(); puts(j); free(j); return 0; }
    if (!strcmp(cmd, "presets")) { for (int p = 0; p < me1_npresets; p++) printf("%-10s %s — %s\n", me1_presets[p].id, me1_presets[p].label, me1_presets[p].help); return 0; }
    me1cfg_load(&c, game, docs);
    if (!strcmp(cmd, "show")) { show(&c); return 0; }
    if (!strcmp(cmd, "dump")) { char* d = me1cfg_dump(&c); fputs(d, stdout); free(d); return 0; }
    if (!strcmp(cmd, "paths")) {
        char u[1024], d[1024];
        for (int f = 0; f < SF_COUNT; f++) { me1cfg_paths(game, docs, f, u, d, sizeof u); printf("%s\t%s\n", d[0] ? d : "-", u); }
        return 0;
    }
    if (!strcmp(cmd, "get")) {
        if (a >= argc) { usage(); return 2; }
        int i; if (!me1cfg_find(argv[a], &i)) { fprintf(stderr, "неизвестная настройка '%s'\n", argv[a]); return 1; }
        puts(c.value[i]); return 0;
    }
    if (!strcmp(cmd, "set")) {
        if (a >= argc) { usage(); return 2; }
        for (; a < argc; a++) {
            char id[128]; const char* eq = strchr(argv[a], '=');
            if (!eq) { fprintf(stderr, "ожидается ID=ЗНАЧ: %s\n", argv[a]); return 2; }
            snprintf(id, sizeof id, "%.*s", (int)(eq - argv[a]), argv[a]);
            if (me1cfg_set(&c, id, eq + 1, err, sizeof err)) { fprintf(stderr, "ошибка: %s\n", err); return 1; }
        }
        return save(&c);
    }
    if (!strcmp(cmd, "preset")) {
        if (a >= argc) { usage(); return 2; }
        if (me1cfg_preset(&c, argv[a], err, sizeof err)) { fprintf(stderr, "ошибка: %s\n", err); return 1; }
        return save(&c);
    }
    if (!strcmp(cmd, "auto")) {   /* auto "Mali-G52 MC2" MT6769 3700 8 2000 1600x720 */
        if (a >= argc) { usage(); return 2; }
        ME1Device d = { argv[a], a + 1 < argc ? argv[a + 1] : "", a + 2 < argc ? atoi(argv[a + 2]) : 0,
                        a + 3 < argc ? atoi(argv[a + 3]) : 0, a + 4 < argc ? atoi(argv[a + 4]) : 0, 0, 0 };
        if (a + 5 < argc) sscanf(argv[a + 5], "%dx%d", &d.screen_w, &d.screen_h);
        char rep[512]; int t = me1cfg_auto(&c, &d, rep, sizeof rep);
        puts(rep);
        return t < 0 ? 1 : save(&c);
    }
    if (!strcmp(cmd, "reset")) { me1cfg_reset(&c); return save(&c); }
    if (!strcmp(cmd, "modgen")) { if (a >= argc) { fprintf(stderr, "modgen: light|balanced|aggressive|extreme|cpu|auto GPU|custom КЛЮЧ=ЗНАЧ...|off\n"); return 2; } return modgen(game, argv[a], argc - a - 1, argv + a + 1); }
    if (!strcmp(cmd, "patch")) {
        if (a >= argc) { usage(); return 2; }
        char u[1024], d[1024]; int f = !strcmp(argv[a], "Engine") ? SF_ENGINE : !strcmp(argv[a], "Game") ? SF_GAME : !strcmp(argv[a], "Input") ? SF_INPUT : -1;
        if (f < 0) { usage(); return 2; }
        me1cfg_paths(game, docs, f, u, d, sizeof u);
        FILE* fp = fopen(d, "rb"); if (!fp) { perror(d); return 1; }
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        char* b = malloc(n + 1); n = fread(b, 1, n, fp); fclose(fp);
        void* o; size_t on;
        if (me1cfg_patch_default(docs, argv[a], b, n, &o, &on)) { fwrite(o, 1, on, stdout); free(o); }
        else fwrite(b, 1, n, stdout);
        free(b); return 0;
    }
    usage(); return 2;
}
