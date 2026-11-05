#!/usr/bin/env bash
# build-android.sh — сборка APK порта Mass Effect (PC, 2007) для Android arm64 одной командой (хост: Linux x86-64, в т. ч. WSL2):
# находит игру и DLL, перекомпилирует изменившиеся модули (x86 → C), собирает NDK-библиотеки и
# android/out/me1port-launcher.apk. Gradle и Android Studio не нужны.
#
#   ./build-android.sh [параметры]
#
#   --setup          скачать недостающее в папку инструментов: JDK 17, Android SDK (build-tools 30.0.3,
#                    platform android-29, cmake), NDK r27c, glslang 15.0.0 под Android (~3 ГБ, один раз)
#   --tools DIR      папка инструментов (по умолчанию $ME1_TOOLS или ~/me1port-tools). Уже установленные
#                    SDK/NDK/JDK тоже находятся: $ANDROID_HOME, $ANDROID_SDK_ROOT, ~/Android/Sdk, $NDK, $JAVA_HOME
#   --deps           поставить системные пакеты (make, zip, unzip, curl, python3 + capstone, JDK 17) через sudo
#   --game DIR       папка игры на ПК (в ней Binaries/MassEffect.exe). Без неё ищется сама: $ME1_GAME,
#                    библиотеки Steam, ~/Games, ~/GOG Games, префиксы Wine
#   --dlls DIR       где взять msvcr80.dll, msvcp80.dll (VC++ 2005) и d3dx9_31.dll (DirectX 9.15.779)
#                    или их установщики (vcredist_x86.exe, directx_Jun2010_redist.exe, *.cab)
#   --no-game        APK без игры: только лаунчер (проверка интерфейса, поиска файлов, настроек)
#   --no-spv         не собирать libme1spv.so (без Vulkan-рендерера; OpenGL ES работает)
#   --gpu            собрать Mesa Zink + libadrenotools (android/gpu/build_gpu.sh: meson, ninja, cmake, bison, flex, git, сеть)
#   --no-gpu         не класть в APK уже собранные GPU-библиотеки
#   --recomp         перекомпилировать все модули заново (по умолчанию — только изменившиеся)
#   --release        MassEffect.exe с -O2: сборка дольше, игра быстрее
#   -j N             параллельных задач make (по умолчанию — число ядер)
#   --clean          удалить build/android и android/build перед сборкой (out/ не трогается)
#   --keystore FILE  свой ключ подписи (пароль хранилища и ключа — android); по умолчанию отладочный ключ из репозитория
#   --install        поставить APK на телефон по adb (включите «Отладку по USB»)
#   -v, --verbose    показывать весь вывод make
#   -h, --help       эта справка
#
# Примеры:
#   ./build-android.sh --setup                   # первый раз: инструменты + сборка
#   ./build-android.sh --game ~/Games/MassEffect --dlls ~/me1-dlls --release --install
#   ./build-android.sh --setup --no-game         # только лаунчер, без файлов игры
# Логи: build/recomp/<модуль>.log, build/logs/*.log

set -u
CALLER=$(pwd)
ROOT=$(cd "$(dirname "$0")" && pwd -P)
cd "$ROOT" || exit 1
if [ ! -f Makefile ] || [ ! -f android/build_apk.sh ]; then echo "build-android.sh должен лежать в корне исходников me1port" >&2; exit 1; fi
usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }
# ---------------------------------------------------------------- общее: вывод, утилиты
if [ -t 1 ]; then BO=$'\033[1m'; GR=$'\033[32m'; YE=$'\033[33m'; RE=$'\033[31m'; NO=$'\033[0m'; TTY=1
else BO=; GR=; YE=; RE=; NO=; TTY=0; fi
say()  { printf '\n%s==> %s%s\n' "$BO" "$*" "$NO"; }
ok()   { printf '  %s✓%s %s\n' "$GR" "$NO" "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '  %s!%s %s\n' "$YE" "$NO" "$*" >&2; }
die()  { printf '\n%sОшибка:%s %s\n' "$RE" "$NO" "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
ME=${USER:-$(id -un 2>/dev/null || echo user)}
# Ctrl+C: фоновые задачи (recomp.py, make) в неинтерактивном bash игнорируют SIGINT — гасим их деревом
kill_tree() { for _c in $(pgrep -P "$1" 2>/dev/null); do kill_tree "$_c"; done; kill -TERM "$1" 2>/dev/null; }
on_int() { trap - INT TERM; for _j in $(jobs -p); do kill_tree "$_j"; done; rm -rf out/.new-* 2>/dev/null; printf '\nпрервано\n' >&2; exit 130; }
trap on_int INT TERM
uniq_lines() { awk 'NF && !seen[$0]++'; }
ncpu() { nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2; }
mem_gb() {
    if [ -r /proc/meminfo ]; then awk '/^MemTotal:/ {print int($2 / 1000000 + 0.5)}' /proc/meminfo
    else echo $(( $(sysctl -n hw.memsize 2>/dev/null || echo 4000000000) / 1000000000 )); fi
}
# sha FILE (или stdin без аргумента)
sha() {
    if have sha256sum; then sha256sum ${1:+"$1"} | cut -d' ' -f1
    else shasum -a 256 ${1:+"$1"} | cut -d' ' -f1; fi
}
# абсолютный путь относительно папки, из которой запустили скрипт
abspath() {
    case $1 in
        /*) printf '%s\n' "$1" ;;
        "~") printf '%s\n' "$HOME" ;;
        "~/"*) printf '%s/%s\n' "$HOME" "${1#\~/}" ;;
        *) printf '%s/%s\n' "$CALLER" "$1" ;;
    esac
}
# ci_path BASE a/b/c — путь без учёта регистра букв (файлы из Windows: Binaries/binaries, MassEffect.exe/masseffect.exe)
ci_path() {
    _p=$1; _r=$2
    [ -d "$_p" ] || return 1
    while [ -n "$_r" ]; do
        _c=${_r%%/*}
        case $_r in */*) _r=${_r#*/} ;; *) _r= ;; esac
        if [ -e "$_p/$_c" ]; then _p=$_p/$_c; continue; fi
        _m=$(find "$_p" -mindepth 1 -maxdepth 1 -iname "$_c" 2>/dev/null | head -n 1)
        [ -n "$_m" ] || return 1
        _p=$_m
    done
    printf '%s\n' "$_p"
}
# ждать фоновый процесс, показывая время: wait_job PID ПОДПИСЬ
wait_job() {
    _pid=$1; _t0=$(date +%s)
    while kill -0 "$_pid" 2>/dev/null; do
        if [ $TTY = 1 ]; then _e=$(( $(date +%s) - _t0 )); printf '\r    %s … %d:%02d ' "$2" $((_e / 60)) $((_e % 60)); fi
        sleep 2
    done
    wait "$_pid"; _rc=$?
    [ $TTY = 1 ] && printf '\r\033[K'
    return $_rc
}
# make с логом и счётчиком объектов: run_make ЛОГ аргументы-make...
run_make() {
    _log=$1; shift
    mkdir -p "$(dirname "$_log")"
    if [ "$VERBOSE" = 1 ]; then
        make "$@" 2>&1 | tee "$_log"; _rc=${PIPESTATUS[0]}
    else
        _total=$(make -n "$@" 2>/dev/null | grep -c -e ' -c ')
        _stamp=$(dirname "$_log")/.stamp; : > "$_stamp"
        make "$@" > "$_log" 2>&1 &
        _pid=$!; _t0=$(date +%s)
        while kill -0 "$_pid" 2>/dev/null; do
            if [ $TTY = 1 ]; then
                _n=$(find build -name '*.o' -newer "$_stamp" 2>/dev/null | wc -l); _e=$(( $(date +%s) - _t0 ))
                printf '\r    компиляция: %d / %d объектов, %d:%02d ' $_n $_total $((_e / 60)) $((_e % 60))
            fi
            sleep 3
        done
        wait "$_pid"; _rc=$?
        [ $TTY = 1 ] && printf '\r\033[K'
    fi
    if [ $_rc -ne 0 ]; then
        printf '\n%s--- make: ошибки (полный лог: %s) ---%s\n' "$RE" "$_log" "$NO" >&2
        grep -i -E 'error|ошибка|undefined reference|No such file' "$_log" | head -n 15 >&2
        printf -- '--- последние строки ---\n' >&2; tail -n 12 "$_log" >&2
    fi
    return $_rc
}

# ---------------------------------------------------------------- поиск игры и DLL
# библиотеки Steam (родной Steam, Flatpak, Snap) с учётом libraryfolders.vdf
steam_libs() {
    for _r in "$HOME/.steam/steam" "$HOME/.steam/root" "$HOME/.local/share/Steam" "$HOME/.steam/debian-installation" \
              "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
        [ -d "$_r/steamapps" ] || continue
        printf '%s\n' "$_r"
        for _v in "$_r/steamapps/libraryfolders.vdf" "$_r/config/libraryfolders.vdf"; do
            [ -f "$_v" ] && sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$_v"
        done
    done | while IFS= read -r _l; do
        if [ -d "$_l/steamapps" ]; then (cd "$_l" && pwd -P); fi
    done | uniq_lines
    return 0
}
# префиксы Wine/Proton и разделы Windows, где могут лежать настоящие msvcr80/msvcp80/d3dx9_31
win_roots() {
    steam_libs | while IFS= read -r _l; do
        [ -d "$_l/steamapps/compatdata/17460/pfx/drive_c" ] && printf '%s\n' "$_l/steamapps/compatdata/17460/pfx/drive_c"
    done
    [ -n "${WINEPREFIX:-}" ] && [ -d "$WINEPREFIX/drive_c" ] && printf '%s\n' "$WINEPREFIX/drive_c"
    [ -d "$HOME/.wine/drive_c" ] && printf '%s\n' "$HOME/.wine/drive_c"
    for _d in "$HOME"/Games/*/drive_c "$HOME"/.local/share/wineprefixes/*/drive_c "$HOME"/Games/Heroic/Prefixes/*/*/drive_c \
              /mnt/* /media/"$ME"/* /run/media/"$ME"/*; do
        [ -d "$_d" ] && ci_path "$_d" Windows >/dev/null && printf '%s\n' "$_d"
    done
    return 0
}
game_candidates() {
    [ -n "${ME1_GAME:-}" ] && printf '%s\n' "$ME1_GAME"
    _cfg=${XDG_CONFIG_HOME:-$HOME/.config}/me1port/game_dir
    [ -f "$_cfg" ] && head -n 1 "$_cfg"
    steam_libs | while IFS= read -r _l; do
        for _n in "Mass Effect" "MassEffect"; do [ -d "$_l/steamapps/common/$_n" ] && printf '%s\n' "$_l/steamapps/common/$_n"; done
    done
    for _r in "$HOME/Games" "$HOME/games" "$HOME/GOG Games" "$HOME/.local/share/lutris" "${WINEPREFIX:-$HOME/.wine}/drive_c" "$HOME/.wine/drive_c"; do
        [ -d "$_r" ] && find "$_r" -maxdepth 9 -ipath '*/binaries/masseffect.exe' 2>/dev/null | sed 's|/[^/]*/[^/]*$||'
    done
    return 0
}
# проверка PE: pe_check ФАЙЛ crt|cpp|dx|exe|dll -> печатает версию (или причину отказа)
pe_check() {
    "$PY" - "$1" "$2" <<'PYEOF'
import sys, re
f, kind = sys.argv[1], sys.argv[2]
try:
    d = open(f, 'rb').read()
except Exception as e:
    print('не читается (%s)' % e); sys.exit(1)
def bad(msg): print(msg); sys.exit(1)
if d[:2] != b'MZ': bad('не PE-файл')
if b'Wine builtin DLL' in d[:4096] or b'Wine placeholder DLL' in d[:4096]: bad('это заглушка Wine, а не файл Microsoft')
o = int.from_bytes(d[0x3c:0x40], 'little')
if d[o:o + 4] != b'PE\0\0': bad('не PE-файл')
if int.from_bytes(d[o + 4:o + 6], 'little') != 0x14c: bad('не 32-битный x86 (нужна 32-битная версия)')
def fileversion():
    k = 'FileVersion'.encode('utf-16-le') + b'\0\0'
    i = d.find(k)
    if i < 0: return ''
    i += len(k)
    while d[i:i + 2] == b'\0\0': i += 2
    j = i
    while j + 1 < len(d) and d[j:j + 2] != b'\0\0' and j - i < 128: j += 2
    return d[i:j].decode('utf-16-le', 'replace').strip()
v = fileversion()
need = {'crt': (8, 0, 50727), 'cpp': (8, 0, 50727), 'dx': (9, 15, 779)}.get(kind)
if need:
    nums = tuple(int(x) for x in re.findall(r'\d+', v)[:3])
    if nums != need:
        bad('версия %s, нужна %s' % (v or '?', {'crt': '8.00.50727.x (VC++ 2005)', 'cpp': '8.00.50727.x (VC++ 2005)', 'dx': '9.15.779 (DirectX, d3dx9_31)'}[kind]))
print(v or 'ok')
PYEOF
}
# распаковка установщиков (cab, directx_*_redist.exe, vcredist_x86.exe): xtract АРХИВ ПАПКА [фильтр файлов верхнего уровня]
xtract() {
    mkdir -p "$2"
    if have cabextract; then cabextract -q ${3:+-F "$3"} -d "$2" "$1" >/dev/null 2>&1
    elif have 7z; then 7z x -y -o"$2" "$1" ${3:+-r "$3"} >/dev/null 2>&1
    elif have bsdtar; then bsdtar -xf "$1" -C "$2" >/dev/null 2>&1
    else return 2; fi
    find "$2" -type f -iname '*.cab' 2>/dev/null | while IFS= read -r _c; do
        _o=${_c%.*}.d; [ -d "$_o" ] && continue
        mkdir -p "$_o"
        if have cabextract; then cabextract -q -d "$_o" "$_c" >/dev/null 2>&1
        elif have 7z; then 7z x -y -o"$_o" "$_c" >/dev/null 2>&1
        else bsdtar -xf "$_c" -C "$_o" >/dev/null 2>&1; fi
    done
    return 0
}
REDIST_DONE=0
extract_redists() {
    [ $REDIST_DONE = 1 ] && return 0
    REDIST_DONE=1
    _list=$(
        for _d in "${DLLS:-}" "$GAME" "$HOME/Downloads" "$HOME/Загрузки"; do
            [ -n "$_d" ] && [ -d "$_d" ] || continue
            find "$_d" -maxdepth 5 -type f \( -iname '*d3dx9_31*x86*.cab' -o -iname 'directx*redist*.exe' -o -iname 'dxsetup*.exe' \
                -o -iname 'vcredist_x86*.exe' -o -iname 'vcredist*2005*.exe' \) 2>/dev/null
        done | uniq_lines)
    [ -n "$_list" ] || return 0
    if ! have cabextract && ! have 7z && ! have bsdtar; then
        warn "найдены установщики DirectX/VC++, но нечем распаковать — поставьте cabextract (или p7zip / libarchive)"; return 0
    fi
    while IFS= read -r _a; do
        _o=build/redist/$(basename "$_a").d
        [ -d "$_o" ] && continue
        info "распаковываю $(basename "$_a")"
        case $(basename "$_a" | tr 'A-Z' 'a-z') in
            *.cab) xtract "$_a" "$_o" ;;
            directx*|dxsetup*) xtract "$_a" "$_o" '*d3dx9_31*x86*.cab' ;;
            *) xtract "$_a" "$_o" ;;
        esac
    done <<EOF
$_list
EOF
    return 0
}
dll_candidates() { # ИМЯ.dll local|win — сначала папка --dlls и игра, потом префиксы Wine/Proton и Windows
    if [ "$2" = local ]; then
        if [ -n "${DLLS:-}" ]; then ci_path "$DLLS" "$1"; find "$DLLS" -maxdepth 3 -type f -iname "$1" 2>/dev/null; fi
        ci_path "$BIN" "$1"
        find "$GAME" -maxdepth 5 -type f -iname "$1" 2>/dev/null
        return 0
    fi
    win_roots | while IFS= read -r _w; do
        _win=$(ci_path "$_w" Windows) || continue
        _sxs=$(ci_path "$_win" WinSxS) && find "$_sxs" -maxdepth 2 -type f -ipath '*x86_microsoft.vc80.crt_*' -iname "$1" 2>/dev/null
        ci_path "$_win" "SysWOW64/$1"
        ci_path "$_win" "System32/$1"
    done
    return 0
}
# try_dlls ИМЯ ВИД < список файлов -> FOUND
try_dlls() {
    while IFS= read -r _f; do
        [ -n "$_f" ] && [ -f "$_f" ] || continue
        if _v=$(pe_check "$_f" "$2"); then FOUND=$_f; ok "$1: $_f ($_v)"; return 0; fi
        info "пропущен $_f: $_v"
    done
    return 1
}
# find_dll ИМЯ.dll ВИД -> путь в FOUND (установщики распаковываются, только если готового файла нет)
find_dll() {
    FOUND=
    dll_candidates "$1" local | uniq_lines > build/.cand
    try_dlls "$1" "$2" < build/.cand && return 0
    dll_candidates "$1" win | uniq_lines > build/.cand
    try_dlls "$1" "$2" < build/.cand && return 0
    extract_redists
    [ -d build/redist ] || return 1
    find build/redist -type f -iname "*${1%.*}*" 2>/dev/null > build/.cand
    try_dlls "$1" "$2" < build/.cand
}
find_game() {
    GAME=; BIN=; EXE=
    if [ -n "$GAME_ARG" ]; then
        _g=$(abspath "$GAME_ARG")
        [ -f "$_g" ] && _g=$(dirname "$_g")                       # указали сам MassEffect.exe
        case $(basename "$_g" | tr 'A-Z' 'a-z') in binaries) _g=$(dirname "$_g") ;; esac
        EXE=$(ci_path "$_g" Binaries/MassEffect.exe) || die "в «$_g» нет Binaries/MassEffect.exe — укажите папку установленной Mass Effect (PC, 2007)"
        GAME=$_g
    else
        while IFS= read -r _g; do
            if EXE=$(ci_path "$_g" Binaries/MassEffect.exe); then GAME=$_g; break; fi
        done <<EOF
$(game_candidates | uniq_lines)
EOF
    fi
    [ -n "$GAME" ] || return 1
    GAME=$(cd "$GAME" && pwd -P); BIN=$(dirname "$EXE")
    return 0
}

# ---------------------------------------------------------------- перекомпиляция x86 -> C (out/<модуль>)
# модуль:файл:параметры recomp.py (файл crt/cpp/dx — найденные DLL, остальные — из Binaries/)
MODULES="msvcr80:crt:--prefix crt
msvcp80:cpp:--prefix cpp
physxloader:PhysXLoader.dll:--prefix pxl
physxcore:PhysXCore.dll:--prefix pxc --base 12000000
ogg:ogg.dll:--prefix ogg --base 10100000
vorbis:vorbis.dll:--prefix vb --base 10200000
vorbisfile:vorbisfile.dll:--prefix vf --base 10300000
bink:binkw32.dll:--prefix bk --base 10400000
d3dx9:dx:--prefix dx --code-ranges 2b005-21e0a8
exe:MassEffect.exe:--prefix m --seeds @SEEDS@"
MOD_NAMES="msvcr80 msvcp80 physxloader physxcore ogg vorbis vorbisfile bink d3dx9 exe"
out_complete() { for _m in $MOD_NAMES; do [ -f "out/$_m/image.bin" ] || return 1; done; return 0; }

find_python() {
    # python3 из PATH, системный (пакет python3-capstone ставится для него, а в PATH бывает pyenv/conda), окружение с pip
    _venv=${XDG_CACHE_HOME:-$HOME/.cache}/me1port/pyenv
    for PY in ${ME1_PYTHON:-} python3 /usr/bin/python3 $(ls /usr/bin/python3.[0-9]* 2>/dev/null | grep -v -e config -e '\.[0-9]*m$') "$_venv/bin/python"; do
        have "$PY" && "$PY" -c 'import capstone' 2>/dev/null && return 0
    done
    PY=python3
    have python3 || return 1
    [ "${1:-}" = install ] || return 1
    info "capstone нет — ставлю в отдельное окружение $_venv (pip install capstone)"
    { python3 -m venv "$_venv" && "$_venv/bin/python" -m pip install -q capstone; } >/dev/null 2>&1 || return 1
    PY=$_venv/bin/python
    return 0
}
recomp_job() { # МОДУЛЬ ФАЙЛ КЛЮЧ параметры...
    _m=$1 _src=$2 _key=$3; shift 3
    rm -rf "out/.new-$_m"
    "$PY" tools/recomp.py "$_src" "out/.new-$_m" "$@" > "build/recomp/$_m.log" 2>&1 || return 1
    [ -f "out/.new-$_m/image.bin" ] || return 1
    rm -rf "out/$_m" && mv "out/.new-$_m" "out/$_m" && printf '%s\n' "$_key" > "out/$_m/.build-key"
}
recompile() {
    mkdir -p out build/recomp build/seeds
    # seeds (начала функций за таблицей переходов инкрементальной линковки) привязаны к сборке EXE
    _es=$(sha "$EXE")
    SEEDS=build/seeds/$_es.txt
    if [ ! -s "$SEEDS" ]; then
        info "seeds для MassEffect.exe (tools/ilt_seeds.py)…"
        "$PY" tools/ilt_seeds.py "$EXE" "$SEEDS" > "build/seeds/$_es.log" 2>&1 || { rm -f "$SEEDS"; die "tools/ilt_seeds.py: см. build/seeds/$_es.log"; }
    fi
    if cmp -s "$SEEDS" tools/seeds/MassEffect_ilt_funcs.txt; then SEEDS=tools/seeds/MassEffect_ilt_funcs.txt; ok "MassEffect.exe — та же сборка, что у автора порта"
    else warn "MassEffect.exe другой сборки — seeds сгенерированы заново ($SEEDS); если игра падает, сообщите версию EXE"; fi
    _th=$(cat tools/recomp.py tools/fpsse.py tools/ehscan.py tools/pefile_min.py | sha)
    _todo=
    while IFS=: read -r _m _f _a; do
        case $_f in crt) _s=$CRT ;; cpp) _s=$CPP ;; dx) _s=$DX ;; *) _s=$(ci_path "$BIN" "$_f") || _s=$(find "$GAME" -maxdepth 5 -type f -iname "$_f" 2>/dev/null | head -n 1) ;; esac
        [ -n "$_s" ] && [ -f "$_s" ] || die "нет $_f в $BIN — проверьте целостность файлов игры"
        _sa=$(sha "$_s")
        _kk="$_sa;$_a;$_th"; [ "$_m" = exe ] && _kk="$_kk;$(sha "$SEEDS")"
        _a=$(printf '%s' "$_a" | sed "s|@SEEDS@|$SEEDS|")
        if [ "$FORCE" = 0 ] && [ -f "out/$_m/image.bin" ]; then
            if [ ! -f "out/$_m/.build-key" ]; then ok "$_m: уже есть (out/$_m, собран вручную — оставляю; --recomp пересоздаст)"; continue; fi
            if [ "$(cat "out/$_m/.build-key")" = "$_kk" ]; then ok "$_m: актуален"; continue; fi
        fi
        _todo="$_todo$_m|$_s|$_kk|$_a
"
    done <<EOF
$MODULES
EOF
    [ -n "$_todo" ] || return 0
    # MassEffect.exe — самый долгий (10–20 мин): при достатке памяти идёт параллельно с остальными
    _exe_pid=
    _exe_line=$(printf '%s' "$_todo" | grep '^exe|' || true)
    if [ -n "$_exe_line" ] && [ "$(mem_gb)" -ge 6 ] && [ "$(ncpu)" -ge 2 ]; then
        IFS='|' read -r _m _s _kk _a <<EOF
$_exe_line
EOF
        info "exe: перекомпиляция MassEffect.exe в фоне (~10–20 мин, лог build/recomp/exe.log)"
        recomp_job exe "$_s" "$_kk" $_a &
        _exe_pid=$!
    fi
    while IFS='|' read -r _m _s _kk _a; do
        [ -n "$_m" ] || continue
        [ "$_m" = exe ] && [ -n "$_exe_pid" ] && continue
        [ "$_m" = exe ] && info "exe: перекомпиляция MassEffect.exe (~10–20 мин)"
        _rt0=$(date +%s)
        recomp_job "$_m" "$_s" "$_kk" $_a &
        if wait_job $! "$_m"; then ok "$_m: перекомпилирован за $(( $(date +%s) - _rt0 )) с"
        else [ -n "$_exe_pid" ] && kill "$_exe_pid" 2>/dev/null; tail -n 15 "build/recomp/$_m.log" >&2; die "перекомпиляция $_m не удалась (лог build/recomp/$_m.log)"; fi
    done <<EOF
$_todo
EOF
    if [ -n "$_exe_pid" ]; then
        if wait_job "$_exe_pid" "exe (MassEffect.exe)"; then ok "exe: перекомпилирован"
        else tail -n 15 build/recomp/exe.log >&2; die "перекомпиляция MassEffect.exe не удалась (лог build/recomp/exe.log)"; fi
    fi
    rm -rf out/.new-* 2>/dev/null
    return 0
}
# найти игру, DLL и перекомпилировать то, что изменилось
prepare_modules() {
    say "Файлы игры"
    if ! find_game; then
        if out_complete && [ "$FORCE" = 0 ]; then
            warn "папка игры не найдена — использую уже перекомпилированные модули из out/"
            return 0
        fi
        die "не найдена установленная Mass Effect (PC, 2007). Укажите папку: --game \"/путь/Mass Effect\" (в ней Binaries/MassEffect.exe)"
    fi
    ok "игра: $GAME"
    case $GAME in *[Ll]egendary*) warn "похоже на Legendary Edition — порт рассчитан на оригинальную PC-версию 2007 года" ;; esac
    _v=$(pe_check "$EXE" exe) || die "$EXE: $_v"
    ok "MassEffect.exe: $_v"
    [ -n "$DLLS_ARG" ] && { DLLS=$(abspath "$DLLS_ARG"); [ -d "$DLLS" ] || die "--dlls: нет папки $DLLS"; }
    mkdir -p build
    _miss=
    find_dll msvcr80.dll crt && CRT=$FOUND || _miss="$_miss msvcr80.dll"
    find_dll msvcp80.dll cpp && CPP=$FOUND || _miss="$_miss msvcp80.dll"
    find_dll d3dx9_31.dll dx && DX=$FOUND || _miss="$_miss d3dx9_31.dll"
    if [ -n "$_miss" ]; then
        die "не найдены подходящие:$_miss
  Где взять (32-битные файлы Microsoft, не заглушки Wine):
    msvcr80.dll, msvcp80.dll — Visual C++ 2005 (8.00.50727.x): C:\\Windows\\WinSxS\\x86_microsoft.vc80.crt_*\\ на ПК с Windows
        или установщик vcredist_x86.exe (VC++ 2005 SP1 Redistributable);
    d3dx9_31.dll — DirectX 9.15.779: C:\\Windows\\SysWOW64\\ (или System32 на 32-битной Windows)
        или установщик directx_Jun2010_redist.exe (в нём OCT2006_d3dx9_31_x86.cab).
  Сложите файлы или установщики в одну папку и запустите с --dlls ПАПКА
  (установщики ищутся и в ~/Downloads; для распаковки нужен cabextract)."
    fi
    say "Перекомпиляция модулей (x86 → C, out/)"
    recompile
}

# ---------------------------------------------------------------- параметры
GAME_ARG= DLLS_ARG= DLLS= TOOLS_ARG= SETUP=0 DEPS=0 NOGAME=0 NOSPV=0 GPU=0 NOGPU=0 FORCE=0 RELEASE=0 JOBS= CLEAN=0
KEYSTORE_ARG= ADBINST=0 VERBOSE=0
while [ $# -gt 0 ]; do
    case $1 in
        --setup)      SETUP=1 ;;
        --deps)       DEPS=1 ;;
        --tools)      [ $# -ge 2 ] || die "--tools: укажите папку"; TOOLS_ARG=$2; shift ;;
        --tools=*)    TOOLS_ARG=${1#*=} ;;
        --game)       [ $# -ge 2 ] || die "--game: укажите папку"; GAME_ARG=$2; shift ;;
        --game=*)     GAME_ARG=${1#*=} ;;
        --dlls)       [ $# -ge 2 ] || die "--dlls: укажите папку"; DLLS_ARG=$2; shift ;;
        --dlls=*)     DLLS_ARG=${1#*=} ;;
        --no-game)    NOGAME=1 ;;
        --no-spv)     NOSPV=1 ;;
        --gpu)        GPU=1 ;;
        --no-gpu)     NOGPU=1 ;;
        --recomp)     FORCE=1 ;;
        --release)    RELEASE=1 ;;
        -j)           [ $# -ge 2 ] || die "-j: укажите число"; JOBS=$2; shift ;;
        -j*)          JOBS=${1#-j} ;;
        --jobs=*)     JOBS=${1#*=} ;;
        --clean)      CLEAN=1 ;;
        --keystore)   [ $# -ge 2 ] || die "--keystore: укажите файл"; KEYSTORE_ARG=$2; shift ;;
        --keystore=*) KEYSTORE_ARG=${1#*=} ;;
        --install)    ADBINST=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "неизвестный параметр: $1 (справка: $0 -h)" ;;
    esac
    shift
done
[ -n "$JOBS" ] || JOBS=$(ncpu)
case $JOBS in ''|*[!0-9]*|0) die "-j: нужно целое число больше нуля" ;; esac
[ $GPU = 1 ] && [ $NOGPU = 1 ] && die "--gpu и --no-gpu вместе не имеют смысла"
TOOLS=$(abspath "${TOOLS_ARG:-${ME1_TOOLS:-$HOME/me1port-tools}}")
case $TOOLS in *" "*) die "папка инструментов не должна содержать пробелов (Makefile): $TOOLS — укажите другую через --tools" ;; esac

# ---------------------------------------------------------------- система
say "Система"
[ "$(uname -s)" = Linux ] || die "сборка APK идёт на Linux x86-64 (подойдёт WSL2 в Windows); здесь $(uname -s)"
case $(uname -m) in
    x86_64|amd64) ;;
    *) die "нужен x86-64: Android NDK не выпускается для Linux $(uname -m). Соберите APK на x86-64 ПК (или в WSL2)" ;;
esac
DISTRO=$( . /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-${ID:-Linux}}" )
ok "$DISTRO, x86_64, ядер: $(ncpu), память: $(mem_gb) ГБ; инструменты: $TOOLS"

pm_packages() {
    _gpu=
    if have apt-get; then PM="apt-get install -y"
        PKGS="make zip unzip curl tar python3 python3-venv python3-capstone openjdk-17-jdk-headless"
        [ $GPU = 1 ] && _gpu="meson ninja-build cmake bison flex git pkg-config python3-mako python3-yaml python3-packaging glslang-tools"
    elif have dnf; then PM="dnf install -y"
        PKGS="make zip unzip curl tar python3 python3-capstone java-17-openjdk-devel"
        [ $GPU = 1 ] && _gpu="meson ninja-build cmake bison flex git pkgconf-pkg-config python3-mako python3-pyyaml python3-packaging glslang"
    elif have pacman; then PM="pacman -S --needed --noconfirm"
        PKGS="make zip unzip curl tar python python-capstone jdk17-openjdk"
        [ $GPU = 1 ] && _gpu="meson ninja cmake bison flex git pkgconf python-mako python-yaml python-packaging glslang"
    elif have zypper; then PM="zypper --non-interactive install"
        PKGS="make zip unzip curl tar python3 python3-capstone java-17-openjdk-devel"
        [ $GPU = 1 ] && _gpu="meson ninja cmake bison flex git pkg-config python3-Mako python3-PyYAML python3-packaging glslang-devel"
    else PM=; PKGS="make zip unzip curl python3 + capstone, JDK 17"; fi
    PKGS="$PKGS${_gpu:+ $_gpu}"
}
install_deps() {
    pm_packages
    [ -n "$PM" ] || { warn "пакетный менеджер не опознан — поставьте вручную: $PKGS"; return 0; }
    SUDO=
    if [ "$(id -u)" != 0 ]; then
        if have sudo; then SUDO=sudo; elif have doas; then SUDO=doas; else die "нужен sudo (или запустите от root): $PM $PKGS"; fi
    fi
    say "Зависимости: $PM $PKGS"
    case $PM in apt-get*) $SUDO apt-get update -q || true ;; esac
    if ! $SUDO $PM $PKGS; then
        warn "одним списком не встало — ставлю по одному пакету"
        for _p in $PKGS; do if $SUDO $PM "$_p" >/dev/null 2>&1; then ok "$_p"; else warn "пакет $_p не найден — пропускаю"; fi; done
    fi
}
[ $DEPS = 1 ] && install_deps

say "Инструменты"
MISS=
for t in make zip find; do have $t || MISS="$MISS $t"; done
have unzip || have python3 || MISS="$MISS unzip"
if [ $SETUP = 1 ] && ! have curl && ! have wget; then MISS="$MISS curl"; fi
if [ $NOGAME = 0 ]; then
    if find_python $([ $SETUP = 1 ] || [ $DEPS = 1 ] && echo install); then ok "python: $("$PY" --version 2>&1), capstone $("$PY" -c 'import capstone; print(capstone.__version__)' 2>/dev/null)"
    elif have python3; then MISS="$MISS python3-capstone"
    else MISS="$MISS python3 python3-capstone"; fi
fi
if [ -n "$MISS" ]; then
    pm_packages
    die "не хватает:$MISS
  Запустите с --deps или поставьте сами: ${PM:+sudo $PM }$PKGS"
fi

dl() { # URL ФАЙЛ
    info "скачиваю $1"
    mkdir -p "$(dirname "$2")"
    if have curl; then curl -fL --retry 3 --progress-bar -o "$2.part" "$1" || return 1
    else wget -q --show-progress -O "$2.part" "$1" || return 1; fi
    mv -f "$2.part" "$2"
}
unpack_zip() { # ZIP ПАПКА
    mkdir -p "$2"
    if have unzip; then unzip -q -o "$1" -d "$2"
    else python3 -m zipfile -e "$1" "$2" && find "$2" -path '*/bin/*' -type f -exec chmod +x {} +; fi
}
verge() { # verge A B: A >= B (числа через точку)
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -n 1)" = "$2" ]
}

# --- JDK 17+
jdk_major() { "$1/bin/javac" -version 2>&1 | sed -n 's/^javac \([0-9][0-9]*\).*/\1/p' | head -n 1; }
find_jdk() {
    JH=; JV=
    _jc=; have javac && _jc=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
    while IFS= read -r _j; do
        [ -n "$_j" ] && [ -x "$_j/bin/javac" ] || continue
        _v=$(jdk_major "$_j"); [ -n "$_v" ] && [ "$_v" -ge 17 ] 2>/dev/null || continue
        JH=$_j; JV=$_v; return 0
    done <<EOF
${JAVA_HOME:-}
$TOOLS/jdk-17
$_jc
$(ls -d /usr/lib/jvm/*17* /usr/lib/jvm/* /opt/jdk* 2>/dev/null)
EOF
    return 1
}
setup_jdk() {
    say "Скачиваю JDK 17 (Eclipse Temurin) → $TOOLS/jdk-17"
    dl "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse" "$TOOLS/dl/jdk17.tar.gz" || die "не скачался JDK 17"
    rm -rf "$TOOLS/.jdk-tmp" && mkdir -p "$TOOLS/.jdk-tmp" && tar -xzf "$TOOLS/dl/jdk17.tar.gz" -C "$TOOLS/.jdk-tmp" || die "не распаковался JDK"
    rm -rf "$TOOLS/jdk-17" && mv "$TOOLS/.jdk-tmp"/jdk-17* "$TOOLS/jdk-17" && rm -rf "$TOOLS/.jdk-tmp"
}

# --- Android SDK: build-tools, android.jar, NDK
bt_ok() { for _f in d8 aapt2 zipalign apksigner core-lambda-stubs.jar; do [ -e "$1/$_f" ] || return 1; done; return 0; }
ndk_ok() { [ -x "$1/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang" ]; }
find_sdk() {
    SDK=
    for _s in "${ANDROID_SDK:-}" "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$TOOLS/android-sdk" "$HOME/Android/Sdk" \
              /opt/android-sdk /usr/lib/android-sdk /data/tools/android-sdk; do
        [ -n "$_s" ] || continue
        if [ -d "$_s/build-tools" ] || [ -d "$_s/platforms" ] || [ -d "$_s/cmdline-tools" ]; then SDK=$_s; return 0; fi
    done
    return 1
}
find_android_parts() {
    BT=; AJAR=; NDK_DIR=
    if [ -n "${ANDROID_BT:-}" ] && bt_ok "$ANDROID_BT"; then BT=$ANDROID_BT
    elif [ -n "$SDK" ]; then
        if bt_ok "$SDK/build-tools/30.0.3"; then BT=$SDK/build-tools/30.0.3
        else
            for _v in $(ls "$SDK/build-tools" 2>/dev/null | sort -t. -k1,1nr -k2,2nr -k3,3nr); do
                bt_ok "$SDK/build-tools/$_v" && { BT=$SDK/build-tools/$_v; break; }
            done
        fi
    fi
    if [ -n "${ANDROID_JAR:-}" ] && [ -f "$ANDROID_JAR" ]; then AJAR=$ANDROID_JAR
    elif [ -n "$SDK" ]; then
        if [ -f "$SDK/platforms/android-29/android.jar" ]; then AJAR=$SDK/platforms/android-29/android.jar
        else
            for _v in $(ls "$SDK/platforms" 2>/dev/null | sed -n 's/^android-\([0-9][0-9]*\)$/\1/p' | sort -nr); do
                [ "$_v" -ge 29 ] && [ -f "$SDK/platforms/android-$_v/android.jar" ] && { AJAR=$SDK/platforms/android-$_v/android.jar; break; }
            done
        fi
    fi
    while IFS= read -r _n; do
        [ -n "$_n" ] && ndk_ok "$_n" && { NDK_DIR=$_n; break; }
    done <<EOF
${NDK:-}
${ANDROID_NDK_HOME:-}
${ANDROID_NDK_ROOT:-}
${ANDROID_NDK:-}
${SDK:+$SDK/ndk/27.2.12479018}
$( [ -n "$SDK" ] && ls -d "$SDK"/ndk/* 2>/dev/null | sort -r)
${SDK:+$SDK/ndk-bundle}
$TOOLS/android-ndk-r27c
$(ls -d /opt/android-ndk* 2>/dev/null)
/data/tools/android-ndk-r27c
EOF
    return 0
}
find_sdkmanager() {
    SDKM=
    for _m in "$SDK/cmdline-tools/latest/bin/sdkmanager" "$SDK"/cmdline-tools/*/bin/sdkmanager "$SDK/tools/bin/sdkmanager"; do
        [ -x "$_m" ] && { SDKM=$_m; return 0; }
    done
    have sdkmanager && { SDKM=$(command -v sdkmanager); return 0; }
    return 1
}
setup_sdk() { # пакеты sdkmanager...
    if [ -z "$SDK" ] || [ ! -w "$SDK" ]; then
        [ -n "$SDK" ] && warn "$SDK недоступен для записи — ставлю отдельный SDK в $TOOLS/android-sdk"
        SDK=$TOOLS/android-sdk
        set -- $(printf '%s\n' "build-tools;30.0.3" "platforms;android-29" "$@" | uniq_lines)
    fi
    mkdir -p "$SDK"
    if ! find_sdkmanager; then
        say "Скачиваю Android SDK command-line tools → $SDK"
        dl "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip" "$TOOLS/dl/cmdline-tools.zip" || die "не скачались cmdline-tools"
        rm -rf "$SDK/cmdline-tools/.tmp" && unpack_zip "$TOOLS/dl/cmdline-tools.zip" "$SDK/cmdline-tools/.tmp" || die "не распаковались cmdline-tools"
        rm -rf "$SDK/cmdline-tools/latest" && mv "$SDK/cmdline-tools/.tmp/cmdline-tools" "$SDK/cmdline-tools/latest" && rm -rf "$SDK/cmdline-tools/.tmp"
        SDKM=$SDK/cmdline-tools/latest/bin/sdkmanager
    fi
    say "sdkmanager: $*"
    yes 2>/dev/null | JAVA_HOME=$JH "$SDKM" --sdk_root="$SDK" --licenses >/dev/null 2>&1
    JAVA_HOME=$JH "$SDKM" --sdk_root="$SDK" "$@" || die "sdkmanager не смог поставить: $* (лицензии: $SDKM --sdk_root=$SDK --licenses)"
}

# --- glslang 15.0.0 под Android (для libme1spv.so)
gl_ok() {
    [ -f "$1/build-android/glslang/libglslang.a" ] && [ -f "$1/build-android/glslang/libglslang-default-resource-limits.a" ] && [ -d "$1/build-android/include" ]
}
find_glslang() {
    GLS=
    for _g in "${GLSLANG_SRC:-}" "$TOOLS/glslang-15.0.0" /data/build3p/glslang-15.0.0; do
        [ -n "$_g" ] && gl_ok "$_g" && { GLS=$_g; return 0; }
    done
    return 1
}
find_cmake() {
    CMAKE=; NINJA=
    if have cmake && verge "$(cmake --version | sed -n 's/^cmake version \([0-9.]*\).*/\1/p')" 3.17; then CMAKE=$(command -v cmake); fi
    have ninja && NINJA=$(command -v ninja)
    for _c in $(ls -d "$SDK"/cmake/* 2>/dev/null | sort -r); do
        [ -z "$CMAKE" ] && [ -x "$_c/bin/cmake" ] && CMAKE=$_c/bin/cmake
        [ -z "$NINJA" ] && [ -x "$_c/bin/ninja" ] && NINJA=$_c/bin/ninja
    done
    [ -n "$CMAKE" ]
}
setup_glslang() {
    _src=$TOOLS/glslang-15.0.0
    say "glslang 15.0.0 под Android → $_src"
    if [ ! -f "$_src/CMakeLists.txt" ]; then
        dl "https://github.com/KhronosGroup/glslang/archive/refs/tags/15.0.0.tar.gz" "$TOOLS/dl/glslang-15.0.0.tar.gz" || die "не скачался glslang"
        tar -xzf "$TOOLS/dl/glslang-15.0.0.tar.gz" -C "$TOOLS" || die "не распаковался glslang"
    fi
    _gen="Unix Makefiles"; [ -n "$NINJA" ] && _gen=Ninja
    mkdir -p build/logs
    {
        "$CMAKE" -S "$_src" -B "$_src/build-android" -G "$_gen" ${NINJA:+-DCMAKE_MAKE_PROGRAM="$NINJA"} \
            -DCMAKE_TOOLCHAIN_FILE="$NDK_DIR/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
            -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_ENABLE_INSTALL=OFF &&
        "$CMAKE" --build "$_src/build-android" --target glslang glslang-default-resource-limits -j "$JOBS"
    } > build/logs/glslang-android.log 2>&1 &
    wait_job $! "glslang" || { tail -n 15 build/logs/glslang-android.log >&2; die "glslang не собрался (лог build/logs/glslang-android.log)"; }
    ok "glslang собран"
}

if [ $SETUP = 1 ]; then
    _free=$(df -Pk "$(dirname "$TOOLS")" 2>/dev/null | awk 'NR == 2 {print int($4 / 1048576)}')
    [ -n "$_free" ] && [ "$_free" -lt 5 ] && warn "свободно ${_free} ГБ — для SDK, NDK и сборки нужно около 5 ГБ"
fi
find_jdk || { [ $SETUP = 1 ] && setup_jdk && find_jdk; } || die "нет JDK 17+ (javac). Запустите с --setup (скачает JDK 17) или --deps, либо укажите JAVA_HOME"
ok "JDK $JV: $JH"
[ "$JV" -ge 22 ] && warn "JDK $JV: build_apk.sh компилирует с -source 8; если javac откажется, поставьте JDK 17 (--setup)"
find_sdk; find_android_parts
_need=
[ -n "$BT" ] || _need="$_need build-tools;30.0.3"
[ -n "$AJAR" ] || _need="$_need platforms;android-29"
[ -n "$NDK_DIR" ] || _need="$_need ndk;27.2.12479018"
GLS=; [ $NOSPV = 0 ] && find_glslang
if [ $SETUP = 1 ] && [ $NOSPV = 0 ] && [ -z "$GLS" ] && ! find_cmake; then _need="$_need cmake;3.22.1"; fi
if [ $ADBINST = 1 ] && ! have adb && [ ! -x "${SDK:-/nonexistent}/platform-tools/adb" ]; then _need="$_need platform-tools"; fi
if [ -n "$_need" ]; then
    [ $SETUP = 1 ] || die "не найдено:$_need
  Запустите ./build-android.sh --setup (скачает в $TOOLS) или укажите ANDROID_HOME / NDK / ANDROID_BT / ANDROID_JAR"
    setup_sdk $_need
    find_android_parts
    [ -n "$BT" ] && [ -n "$AJAR" ] && [ -n "$NDK_DIR" ] || die "после установки всё ещё нет build-tools / android.jar / NDK в $SDK"
fi
case $NDK_DIR in *" "*) die "путь к NDK не должен содержать пробелов (Makefile): $NDK_DIR" ;; esac
ok "SDK: $SDK"
ok "build-tools: $BT"
ok "android.jar: $AJAR"
NDKV=$(sed -n 's/^Pkg.Revision *= *//p' "$NDK_DIR/source.properties" 2>/dev/null)
ok "NDK ${NDKV:-?}: $NDK_DIR"
case $NDKV in 27.*) ;; *) warn "порт проверялся с NDK r27c (27.2.12479018); с другой версией сборка обычно тоже проходит" ;; esac
if [ $NOSPV = 0 ]; then
    if [ -z "$GLS" ] && [ $SETUP = 1 ]; then find_cmake || die "нет cmake 3.17+ (sdkmanager \"cmake;3.22.1\")"; setup_glslang; find_glslang; fi
    case ${GLS:-} in *" "*) warn "путь к glslang с пробелами — Makefile его не примет; libme1spv.so пропускается"; GLS= ;; esac
    if [ -n "$GLS" ]; then ok "glslang: $GLS"
    else warn "нет glslang под Android — libme1spv.so (Vulkan-рендерер) не соберётся, OpenGL ES работает. --setup соберёт glslang"; fi
fi

if [ $CLEAN = 1 ]; then say "Очистка"; rm -rf build/android android/build; ok "build/android и android/build удалены"; fi
mkdir -p build/logs

# ---------------------------------------------------------------- игра и модули
if [ $NOGAME = 0 ]; then
    prepare_modules
    out_complete || die "в out/ не хватает модулей: $(for m in $MOD_NAMES; do [ -f out/$m/image.bin ] || printf '%s ' $m; done)"
fi

# ---------------------------------------------------------------- сборка
MK=(-j"$JOBS" NDK="$NDK_DIR")
[ $RELEASE = 1 ] && MK+=(EXEOPT=-O2)
T0=$(date +%s)
if [ $NOGAME = 0 ]; then
    say "libme1game.so + libme1run.so (make game-lib)"
    run_make build/logs/make-android-game.log "${MK[@]}" game-lib || die "libme1game.so не собралась"
    ok "build/android/libme1game.so: $(du -h build/android/libme1game.so | cut -f1)"
fi
if [ $NOSPV = 0 ] && [ -n "$GLS" ]; then
    say "libme1spv.so (GLSL → SPIR-V)"
    run_make build/logs/make-android-spv.log "${MK[@]}" GLSLANG_SRC="$GLS" build/android/libme1spv.so || die "libme1spv.so не собралась"
    ok "build/android/libme1spv.so"
fi
if [ $GPU = 1 ]; then
    say "GPU-драйверы: Mesa Zink + libadrenotools (android/gpu/build_gpu.sh, долго)"
    _m=; for t in meson ninja cmake bison flex git pkg-config; do have $t || _m="$_m $t"; done
    python3 -c 'import mako, yaml, packaging' 2>/dev/null || _m="$_m python3-mako/pyyaml/packaging"
    [ -z "$_m" ] || die "для --gpu не хватает:$_m (поставьте их, например, через --deps --gpu)"
    NDK=$NDK_DIR W=$TOOLS/build3p sh android/gpu/build_gpu.sh > build/logs/gpu.log 2>&1 &
    wait_job $! "Mesa + adrenotools" || { tail -n 20 build/logs/gpu.log >&2; die "build_gpu.sh не удался (лог build/logs/gpu.log)"; }
    ok "build/android/gpu/"
fi

say "APK (android/build_apk.sh)"
NO_EXE=; NO_D3DX=; D3DX=
if [ $NOGAME = 1 ]; then NO_EXE=1; NO_D3DX=1
elif [ -n "${DX:-}" ]; then D3DX=$DX
else NO_D3DX=1; warn "d3dx9_31.dll не найдена (игра не указана) — APK без assets/d3dx9_31.dll; укажите --game/--dlls"; fi
KS=; [ -n "$KEYSTORE_ARG" ] && KS=$(abspath "$KEYSTORE_ARG")
rm -f android/out/me1port-launcher.apk
NDK=$NDK_DIR ANDROID_SDK=$SDK ANDROID_BT=$BT ANDROID_JAR=$AJAR JAVA_HOME=$JH \
    NO_EXE=$NO_EXE NO_D3DX=$NO_D3DX D3DX_DLL=${D3DX:-/nonexistent} NO_GPU=$([ $NOGPU = 1 ] && echo 1) KEYSTORE=$KS \
    sh android/build_apk.sh > build/logs/apk.log 2>&1 &
wait_job $! "javac, d8, aapt2, подпись"
APK_RC=$?
grep -E '^\[|^ +\+' build/logs/apk.log | sed 's/^/    /'
if [ $APK_RC -ne 0 ] || [ ! -f android/out/me1port-launcher.apk ]; then
    tail -n 20 build/logs/apk.log >&2; die "APK не собран (лог build/logs/apk.log)"
fi
APK=$ROOT/android/out/me1port-launcher.apk
_e=$(( $(date +%s) - T0 )); ok "$APK ($(du -h "$APK" | cut -f1)), сборка $((_e / 60)):$(printf '%02d' $((_e % 60)))"

if [ $ADBINST = 1 ]; then
    say "Установка на телефон (adb)"
    ADB=$(command -v adb 2>/dev/null || echo "$SDK/platform-tools/adb")
    [ -x "$ADB" ] || die "нет adb (--setup --install поставит platform-tools)"
    if ! "$ADB" devices | awk 'NR > 1 && $2 == "device"' | grep -q .; then
        warn "телефон не виден: подключите USB, включите «Отладку по USB» и подтвердите ключ на экране; затем: $ADB install -r \"$APK\""
    else "$ADB" install -r "$APK" && ok "установлено" || warn "adb install не удался"; fi
fi

# ---------------------------------------------------------------- итог
say "Готово"
info "APK: $APK"
if [ $NOGAME = 1 ]; then
    info "сборка без игры: лаунчер, поиск файлов, настройки; для игры соберите без --no-game"
else
    info "на телефоне: скопируйте папку игры (Binaries, BioGame, …) в /sdcard/MassEffect и откройте лаунчер"
    for _p in "${CRT:-}" "${CPP:-}"; do
        [ -n "$_p" ] || continue
        case $_p in "$BIN"/*) ;; *) info "  $(basename "$_p") взят не из Binaries/ игры — положите его и на телефон: adb push \"$_p\" /sdcard/MassEffect/Binaries/$(basename "$_p" | tr 'A-Z' 'a-z')" ;; esac
    done
    info "не публикуйте этот APK и out/: в них код, полученный из файлов игры"
fi
