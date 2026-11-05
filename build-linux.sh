#!/usr/bin/env bash
# build-linux.sh — сборка порта Mass Effect (PC, 2007) для Linux x86-64 / aarch64 одной командой:
# находит игру и DLL, перекомпилирует изменившиеся модули (x86 → C), собирает me1-linux, лаунчер,
# libme1spv.so и me1cfg в build/linux/ (рядом — images/ с образами модулей).
#
#   ./build-linux.sh [параметры]
#
#   --game DIR       папка игры (в ней Binaries/MassEffect.exe). Без неё ищется сама: $ME1_GAME,
#                    ~/.config/me1port/game_dir, библиотеки Steam, ~/Games, ~/GOG Games, префиксы Wine
#   --dlls DIR       где взять msvcr80.dll, msvcp80.dll (VC++ 2005) и d3dx9_31.dll (DirectX 9.15.779).
#                    Можно положить туда установщики vcredist_x86.exe / directx_Jun2010_redist.exe / *.cab.
#                    Без параметра DLL ищутся в игре, префиксе Proton, ~/.wine, на разделе Windows, в ~/Downloads
#   --demo           собрать только me1-demo и лаунчер (без файлов игры: проверка окна, звука, рендереров)
#   --deps           поставить зависимости (apt, dnf, pacman или zypper; через sudo)
#   --recomp         перекомпилировать все модули заново (по умолчанию — только изменившиеся)
#   --release        MassEffect.exe с -O2: сборка дольше, игра быстрее
#   -j N             параллельных задач make (по умолчанию — число ядер)
#   --clean          удалить build/ перед сборкой (out/ с перекомпилированным кодом не трогается)
#   --install [DIR]  после сборки установить в DIR (по умолчанию ~/.local) с ярлыком «Mass Effect» в меню
#   -v, --verbose    показывать весь вывод make
#   -h, --help       эта справка
#
# Примеры:
#   ./build-linux.sh --deps                      # первый раз: зависимости + сборка
#   ./build-linux.sh --game ~/Games/MassEffect --dlls ~/me1-dlls --release --install
#   ./build-linux.sh --demo                      # без игры
# Логи: build/recomp/<модуль>.log, build/logs/make-linux.log

set -u
CALLER=$(pwd)
ROOT=$(cd "$(dirname "$0")" && pwd -P)
cd "$ROOT" || exit 1
if [ ! -f Makefile ] || [ ! -f tools/recomp.py ]; then echo "build-linux.sh должен лежать в корне исходников me1port" >&2; exit 1; fi
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
GAME_ARG= DLLS_ARG= DLLS= DEMO=0 DEPS=0 FORCE=0 RELEASE=0 JOBS= CLEAN=0 INSTALL=0 PREFIX="$HOME/.local" VERBOSE=0
while [ $# -gt 0 ]; do
    case $1 in
        --game)      [ $# -ge 2 ] || die "--game: укажите папку"; GAME_ARG=$2; shift ;;
        --game=*)    GAME_ARG=${1#*=} ;;
        --dlls)      [ $# -ge 2 ] || die "--dlls: укажите папку"; DLLS_ARG=$2; shift ;;
        --dlls=*)    DLLS_ARG=${1#*=} ;;
        --demo)      DEMO=1 ;;
        --deps)      DEPS=1 ;;
        --recomp)    FORCE=1 ;;
        --release)   RELEASE=1 ;;
        -j)          [ $# -ge 2 ] || die "-j: укажите число"; JOBS=$2; shift ;;
        -j*)         JOBS=${1#-j} ;;
        --jobs=*)    JOBS=${1#*=} ;;
        --clean)     CLEAN=1 ;;
        --install)   INSTALL=1; if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then PREFIX=$(abspath "$2"); shift; fi ;;
        --install=*) INSTALL=1; PREFIX=$(abspath "${1#*=}") ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "неизвестный параметр: $1 (справка: $0 -h)" ;;
    esac
    shift
done
[ -n "$JOBS" ] || JOBS=$(ncpu)
case $JOBS in ''|*[!0-9]*|0) die "-j: нужно целое число больше нуля" ;; esac

# ---------------------------------------------------------------- система
say "Система"
[ "$(uname -s)" = Linux ] || die "build-linux.sh собирает версию для Linux (здесь $(uname -s))"
ARCH=$(uname -m)
case $ARCH in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) die "архитектура $ARCH не поддерживается: нужна x86-64 или aarch64 (64-битная ARM)" ;;
esac
DISTRO=$( . /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-${ID:-Linux}}" )
DID=$( . /etc/os-release 2>/dev/null; echo "${ID:-}" )
ok "$DISTRO, $ARCH, ядер: $(ncpu), память: $(mem_gb) ГБ"
[ "$DID" = steamos ] && warn "SteamOS: корневая система только для чтения — собирайте в контейнере distrobox (Arch или Ubuntu)"

pm_packages() {
    if have apt-get; then PM="apt-get install -y"
        PKGS="build-essential pkg-config python3 python3-venv python3-capstone libsdl2-dev libegl-dev libvulkan-dev glslang-dev spirv-tools cabextract"
    elif have dnf; then PM="dnf install -y"
        PKGS="gcc make pkgconf-pkg-config python3 python3-capstone SDL2-devel mesa-libEGL-devel vulkan-headers glslang-devel spirv-tools-devel cabextract"
    elif have pacman; then PM="pacman -S --needed --noconfirm"
        PKGS="base-devel python python-capstone sdl2 vulkan-headers glslang spirv-tools cabextract"
    elif have zypper; then PM="zypper --non-interactive install"
        PKGS="gcc make pkg-config python3 python3-capstone libSDL2-devel Mesa-libEGL-devel vulkan-devel glslang-devel spirv-tools-devel cabextract"
    else PM=; PKGS="gcc make python3 + capstone, SDL2 (dev), EGL (dev), заголовки Vulkan, glslang + SPIRV-Tools (dev), cabextract"; fi
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
CCX=${CC:-cc}
if have "$CCX"; then ok "компилятор: $("$CCX" --version 2>/dev/null | head -n 1)"
elif have gcc; then CCX=gcc; ok "компилятор: $(gcc --version | head -n 1)"
else MISS="$MISS компилятор-C"; fi
have make && ok "make: $(make --version 2>/dev/null | head -n 1)" || MISS="$MISS make"
MAKEARGS=(-j"$JOBS")
[ "$CCX" != cc ] && [ "$CCX" != "${CC:-}" ] && MAKEARGS+=(CC="$CCX")
if have sdl2-config; then ok "SDL2 $(sdl2-config --version)"
elif have pkg-config && pkg-config --exists sdl2; then
    ok "SDL2 $(pkg-config --modversion sdl2) (pkg-config)"
    MAKEARGS+=(SDL_CFLAGS="$(pkg-config --cflags sdl2)" SDL_LIBS="$(pkg-config --libs sdl2)")
else MISS="$MISS SDL2-dev"; fi
if printf '#include <vulkan/vulkan.h>\n' | "$CCX" -E -x c - >/dev/null 2>&1; then ok "заголовки Vulkan"
else MISS="$MISS заголовки-Vulkan"; fi
if [ $DEMO = 0 ]; then
    if find_python $([ $DEPS = 1 ] && echo install); then ok "python: $("$PY" --version 2>&1), capstone $("$PY" -c 'import capstone; print(capstone.__version__)' 2>/dev/null)"
    elif have python3; then MISS="$MISS python3-capstone"
    else MISS="$MISS python3 python3-capstone"; fi
fi
GL=$(ls /usr/lib*/libglslang* /usr/lib/*/libglslang* /usr/local/lib*/libglslang* 2>/dev/null | head -n 1)
if [ -n "$GL" ] || [ -n "${GLSLANG_LIBS:-}" ]; then ok "glslang: ${GL:-$GLSLANG_LIBS}"
else warn "нет glslang (dev) — Vulkan-рендерер не соберётся, OpenGL ES будет работать (пакет glslang-dev / glslang-devel / glslang)"; fi
if [ -n "$MISS" ]; then
    pm_packages
    die "не хватает:$MISS
  Запустите ./build-linux.sh --deps или поставьте сами: ${PM:+sudo $PM }$PKGS"
fi
if [ $ARCH = aarch64 ] && [ -r /proc/cpuinfo ] && ! grep -qw lrcpc /proc/cpuinfo; then
    MAKEARGS+=(ARM_FLAGS=-march=armv8-a); ok "ARM без RCPC (Raspberry Pi 4 и т. п.): ARM_FLAGS=-march=armv8-a"
fi
[ $RELEASE = 1 ] && MAKEARGS+=(EXEOPT=-O2)

if [ $CLEAN = 1 ]; then say "Очистка"; rm -rf build; ok "build/ удалена"; fi
mkdir -p build/logs

# ---------------------------------------------------------------- игра и модули
if [ $DEMO = 0 ]; then
    prepare_modules
    out_complete || die "в out/ не хватает модулей: $(for m in $MOD_NAMES; do [ -f out/$m/image.bin ] || printf '%s ' $m; done)"
fi

# ---------------------------------------------------------------- сборка
TARGET=linux; [ $DEMO = 1 ] && TARGET=linux-demo
say "Сборка: make ${MAKEARGS[*]} $TARGET"
T0=$(date +%s)
run_make build/logs/make-$TARGET.log "${MAKEARGS[@]}" $TARGET || die "сборка не удалась"
_e=$(( $(date +%s) - T0 )); ok "собрано за $((_e / 60)):$(printf '%02d' $((_e % 60)))"
if [ -f build/linux/libme1spv.so ]; then ok "libme1spv.so: Vulkan-рендерер доступен"
else warn "libme1spv.so не собрана (нет glslang) — только OpenGL ES; подробности: build/linux/libme1spv.log"; fi

if [ $DEMO = 0 ]; then
    for m in $MOD_NAMES; do
        mkdir -p "build/linux/images/$m" && cp -f "out/$m/image.bin" "build/linux/images/$m/image.bin" || die "не скопирован out/$m/image.bin"
    done
    ok "образы модулей: build/linux/images/"
    CFG=${XDG_CONFIG_HOME:-$HOME/.config}/me1port
    if [ -n "${GAME:-}" ] && [ ! -s "$CFG/game_dir" ]; then
        mkdir -p "$CFG" && printf '%s\n' "$GAME" > "$CFG/game_dir" && ok "папка игры запомнена для лаунчера: $CFG/game_dir"
    fi
fi

if [ $INSTALL = 1 ]; then
    if [ $DEMO = 1 ]; then warn "--install пропущен: в режиме --demo игры нет"
    else
        say "Установка в $PREFIX"
        sh linux/install.sh --prefix "$PREFIX" --images out ${GAME:+--game "$GAME"} || die "linux/install.sh завершился с ошибкой"
    fi
fi

# ---------------------------------------------------------------- итог
say "Готово"
if [ $DEMO = 1 ]; then
    info "демо:     mkdir -p /tmp/me1demo/Binaries && build/linux/me1-demo --game /tmp/me1demo --windowed   (Esc — выход)"
    info "лаунчер:  build/linux/me1-launcher"
else
    info "лаунчер:  build/linux/me1-launcher            (настройки, моды, «Запустить игру»)"
    info "сразу:    build/linux/me1-linux${GAME:+ --game \"$GAME\"}"
    [ $INSTALL = 1 ] && info "в меню:   ярлык «Mass Effect» (или $PREFIX/bin/me1-launcher)"
    info "сохранения и логи: ~/.local/share/me1port/Documents"
    info "не публикуйте out/ и build/: там код, полученный из файлов игры"
fi
