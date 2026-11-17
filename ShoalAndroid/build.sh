#!/usr/bin/env bash
# Builds build/Shoal.apk without Gradle. Needs the user-local toolchain in ~/Android (see README.md).
# ./build.sh            build
# ./build.sh install    build, then install on the connected device, or on $ANDROID_SERIAL
#                       (a network device as ip:port, e.g. ANDROID_SERIAL=<device-ip>:5555, is connected first)
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME="${JAVA_HOME:-$HOME/Android/jdk17}"
export PATH="$JAVA_HOME/bin:$PATH"
SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
BT="$SDK/build-tools/34.0.0"
JAR="$SDK/platforms/android-30/android.jar"
B=build

rm -rf "$B/classes" "$B/dex" "$B/gen"
mkdir -p "$B/classes" "$B/dex" "$B/gen" "$B/assets"

# KEEP_SHADERS=1 keeps build/assets/shaders as they are (for experimenting with edited ES shaders on the device).
if [ "${KEEP_SHADERS:-0}" != 1 ]; then
    echo "shaders: ShoalCore/shaders -> GLSL ES 3.10"
    rm -rf "$B/assets/shaders"
    python3 tools/es_shaders.py ../ShoalCore/shaders "$B/assets/shaders"
fi
if command -v glslangValidator >/dev/null; then
    for f in "$B"/assets/shaders/*; do glslangValidator "$f" >/dev/null || { glslangValidator "$f"; exit 1; }; done
fi

"$BT/aapt2" compile --dir res -o "$B/res.zip"
"$BT/aapt2" link -I "$JAR" --manifest AndroidManifest.xml --min-sdk-version 24 --target-sdk-version 30 \
    -A "$B/assets" --java "$B/gen" -o "$B/base.apk" "$B/res.zip"
javac -nowarn -Xlint:-options -source 11 -target 11 -cp "$JAR" -d "$B/classes" \
    $(find src "$B/gen" -name '*.java')
"$BT/d8" --release --lib "$JAR" --min-api 24 --output "$B/dex" $(find "$B/classes" -name '*.class')
python3 - "$B/base.apk" "$B/dex/classes.dex" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "a", zipfile.ZIP_DEFLATED) as z: z.write(sys.argv[2], "classes.dex")
PY
"$BT/zipalign" -f -p 4 "$B/base.apk" "$B/aligned.apk"

KS="$HOME/.android/debug.keystore"
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --out "$B/Shoal.apk" "$B/aligned.apk"
rm -f "$B/aligned.apk" "$B/Shoal.apk.idsig"
echo "built $B/Shoal.apk ($(stat -c %s "$B/Shoal.apk") bytes)"

if [ "${1:-}" = install ]; then
    if [ -n "${ANDROID_SERIAL:-}" ]; then
        case "$ANDROID_SERIAL" in *:*) adb connect "$ANDROID_SERIAL" >/dev/null ;; esac
    elif [ "$(adb devices | grep -c 'device$')" -ne 1 ]; then
        echo "Connect exactly one device (USB or 'adb connect <device-ip>:5555'), or set ANDROID_SERIAL." >&2
        exit 1
    fi
    adb install -r "$B/Shoal.apk"
    # Make Shoal the screensaver, and stop the firmware switching the lamp off / swallowing keys while it runs.
    adb shell settings put secure screensaver_enabled 1
    adb shell settings put secure screensaver_components com.shoal.screensaver/.ShoalDream
    adb shell settings put secure screensaver_default_component com.shoal.screensaver/.ShoalDream
    adb shell "su -c 'setprop persist.sys.screenSaver 0; setprop persist.screensave.running 0'" \
        || echo "WARNING: no root; persist.sys.screenSaver stays 1, so the lamp turns off and only POWER wakes" >&2
    adb shell appops set com.shoal.screensaver WRITE_SETTINGS allow # lets the settings screen change the idle time
    adb shell pm grant com.shoal.screensaver android.permission.WRITE_SECURE_SETTINGS # ...and pick the screensaver
    # TCL's TclAppBoot otherwise refuses to bind the dream on the idle timeout ("no OP_AUTO_START permission").
    adb shell appops set com.shoal.screensaver APP_AUTO_START allow 2>/dev/null || true
    echo "screensaver set; idle timeout is $(( $(adb shell settings get system screen_off_timeout) / 1000 )) s"
fi
