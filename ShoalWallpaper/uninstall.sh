#!/bin/sh
# Removes the Shoal live wallpaper and switches any desktop using it back to the default image wallpaper.
qdbus6 org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript \
    "desktops().forEach(function(d){ if (d.wallpaperPlugin == 'org.shoal.wallpaper') d.wallpaperPlugin = 'org.kde.image'; });"
kpackagetool6 --type Plasma/Wallpaper --remove org.shoal.wallpaper
rm -rf "$HOME/.local/lib/qt6/qml/org/shoal/wallpaper"
rm -f "$HOME/.config/environment.d/shoal-wallpaper.conf"
systemctl --user restart plasma-plasmashell.service
