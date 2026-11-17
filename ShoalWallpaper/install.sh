#!/bin/sh
# Builds and installs the Shoal live wallpaper for the current user (no root needed):
#   QML plugin  -> ~/.local/lib/qt6/qml/org/shoal/wallpaper
#   wallpaper   -> ~/.local/share/plasma/wallpapers/org.shoal.wallpaper
#   import path -> ~/.config/environment.d/shoal-wallpaper.conf (read at login)
# Then restarts plasmashell so it picks everything up.
set -e
cd "$(dirname "$0")"
QMLDIR="$HOME/.local/lib/qt6/qml"

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHOAL_QML_DIR="$QMLDIR" >/dev/null
cmake --build build
cmake --install build

kpackagetool6 --type Plasma/Wallpaper --upgrade package >/dev/null 2>&1 || kpackagetool6 --type Plasma/Wallpaper --install package

mkdir -p "$HOME/.config/environment.d"
printf 'QML_IMPORT_PATH=%s${QML_IMPORT_PATH:+:$QML_IMPORT_PATH}\n' "$QMLDIR" > "$HOME/.config/environment.d/shoal-wallpaper.conf"

# Apply to the running session as well (environment.d is only read at login).
case ":$(systemctl --user show-environment | sed -n 's/^QML_IMPORT_PATH=//p'):" in
    *":$QMLDIR:"*) ;;
    *) systemctl --user set-environment QML_IMPORT_PATH="$QMLDIR" ;;
esac
systemctl --user restart plasma-plasmashell.service
echo "Installed. Right-click the desktop > Desktop and Wallpaper > Wallpaper type: Shoal"
