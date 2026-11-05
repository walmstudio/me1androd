#!/bin/sh
# me1port для Linux: установка в домашнюю папку (или PREFIX) после `make linux`.
#   linux/install.sh [--prefix DIR] [--game ПАПКА_ИГРЫ] [--images DIR]
#     --prefix  куда ставить (по умолчанию ~/.local): bin/me1-launcher, bin/me1-linux, bin/me1cfg,
#               lib/me1port/libme1spv.so, share/applications/me1port.desktop (ярлык запускает лаунчер),
#               share/icons/hicolor/{scalable,128x128}/apps/me1port.{svg,png}
#     --game    запомнить папку игры (~/.config/me1port/game_dir; то же делает лаунчер при выборе папки)
#     --images  скопировать готовые image.bin модулей (out/<модуль>/image.bin) в lib/me1port/images
#   linux/install.sh --uninstall [--prefix DIR]
set -e
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$0")/.."
PREFIX="$HOME/.local"; GAME=""; IMAGES=""; UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --game) GAME="$2"; shift 2 ;;
    --images) IMAGES="$2"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) sed -n '2,9p' "$SELF"; exit 0 ;;
    *) echo "install.sh: неизвестный параметр $1" >&2; exit 2 ;;
    esac
done
BIN="$PREFIX/bin"; LIB="$PREFIX/lib/me1port"; APPS="$PREFIX/share/applications"; ICONS="$PREFIX/share/icons/hicolor/scalable/apps"
ICONS128="$PREFIX/share/icons/hicolor/128x128/apps"
if [ $UNINSTALL = 1 ]; then
    rm -f "$BIN/me1-launcher" "$BIN/me1-linux" "$BIN/me1cfg" "$APPS/me1port.desktop" "$ICONS/me1port.svg" "$ICONS128/me1port.png"
    rm -rf "$LIB"
    echo "удалено из $PREFIX (сохранения и настройки в ~/.local/share/me1port и ~/.config/me1port не тронуты)"
    exit 0
fi
[ -x build/linux/me1-linux ] || { echo "нет build/linux/me1-linux: сначала make linux (см. linux/README.md)" >&2; exit 1; }
mkdir -p "$BIN" "$LIB" "$APPS" "$ICONS" "$ICONS128"
install -m 755 build/linux/me1-linux "$BIN/me1-linux"
if [ -x build/linux/me1-launcher ]; then install -m 755 build/linux/me1-launcher "$BIN/me1-launcher"; LAUNCHER=1
else echo "нет build/linux/me1-launcher (make linux): ярлык запустит игру напрямую" >&2; LAUNCHER=0; fi
[ -x build/linux/me1cfg ] && install -m 755 build/linux/me1cfg "$BIN/me1cfg"
[ -f build/linux/libme1spv.so ] && install -m 755 build/linux/libme1spv.so "$LIB/libme1spv.so"
if [ -n "$IMAGES" ]; then
    for m in exe msvcr80 msvcp80 physxloader physxcore d3dx9 ogg vorbis vorbisfile bink; do
        [ -f "$IMAGES/$m/image.bin" ] && { mkdir -p "$LIB/images/$m"; cp "$IMAGES/$m/image.bin" "$LIB/images/$m/"; }
    done
fi
install -m 644 linux/me1port.svg "$ICONS/me1port.svg"
install -m 644 linux/assets/icon128.png "$ICONS128/me1port.png"
if [ $LAUNCHER = 1 ]; then sed "s|@BIN@|$BIN|g" linux/me1port.desktop > "$APPS/me1port.desktop"
else sed -e "s|@BIN@|$BIN|g" -e "s|/me1-launcher --play|/me1-linux|; s|/me1-launcher|/me1-linux|" linux/me1port.desktop > "$APPS/me1port.desktop"; fi
if [ -n "$GAME" ]; then
    CFG="${XDG_CONFIG_HOME:-$HOME/.config}/me1port"; mkdir -p "$CFG"
    (cd "$GAME" && pwd) > "$CFG/game_dir"
    echo "папка игры: $(cat "$CFG/game_dir")"
fi
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" 2>/dev/null || true
[ -f "$PREFIX/share/icons/hicolor/icon-theme.cache" ] && command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true   # only an existing cache (a new one would go stale)
echo "установлено в $PREFIX: me1-launcher, me1-linux, me1cfg, ярлык «Mass Effect» в меню приложений"
[ $LAUNCHER = 1 ] && echo "запуск: ярлык «Mass Effect» или me1-launcher (сразу в игру: me1-launcher --play)"
case ":$PATH:" in *":$BIN:"*) ;; *) echo "добавьте $BIN в PATH, чтобы запускать me1-launcher / me1-linux из терминала" ;; esac
