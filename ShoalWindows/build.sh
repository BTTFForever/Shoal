#!/usr/bin/env bash
# Cross-compiles the Windows screensaver on Linux: build/Shoal.scr (and the same binary as build/Shoal.exe).
# Needs clang-cl, lld-link and llvm-rc, plus the Windows SDK and MSVC C runtime unpacked by xwin into ~/xwin/sdk
# (`xwin --accept-license --arch x86_64 splat --output ~/xwin/sdk`). The C runtime is linked statically (/MT), so the
# .scr runs without the Visual C++ redistributable.
#   ./build.sh           build
#   ./build.sh deploy    build, copy to the Windows PC ($SHOAL_WIN, an SSH target such as user@windows-pc) and install it
#                        as C:\Windows\System32\Shoal.scr (needs an administrator account there)
set -euo pipefail
cd "$(dirname "$0")"
XWIN="${XWIN:-$HOME/xwin/sdk}"
CORE=../ShoalCore
B=build
mkdir -p "$B"

python3 "$CORE/embed_shaders.py" "$CORE/shaders" "$B/shaders_gen.cpp"

INCLUDES=(/imsvc "$XWIN/crt/include" /imsvc "$XWIN/sdk/include/ucrt" /imsvc "$XWIN/sdk/include/um" /imsvc "$XWIN/sdk/include/shared"
          -I"$CORE" -I"$CORE/include")
CFLAGS=(--target=x86_64-pc-windows-msvc /nologo /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /DNDEBUG /W3
        -Wno-unused-command-line-argument -fms-compatibility-version=19.40)

objs=()
for src in shoal_win.cpp "$CORE/shoal_engine.cpp" "$CORE/shoal_fishshape.cpp" "$CORE/shoal_gl_win.cpp" "$B/shaders_gen.cpp"; do
    obj="$B/$(basename "${src%.cpp}").obj"
    clang-cl "${CFLAGS[@]}" "${INCLUDES[@]}" /c "$src" /Fo"$obj"
    objs+=("$obj")
done

llvm-rc /nologo /I "$XWIN/sdk/include/um" /I "$XWIN/sdk/include/shared" /fo "$B/Shoal.res" Shoal.rc

lld-link /nologo /subsystem:windows /entry:wWinMainCRTStartup /out:"$B/Shoal.scr" \
    /libpath:"$XWIN/crt/lib/x86_64" /libpath:"$XWIN/sdk/lib/um/x86_64" /libpath:"$XWIN/sdk/lib/ucrt/x86_64" \
    "${objs[@]}" "$B/Shoal.res" user32.lib gdi32.lib kernel32.lib advapi32.lib comctl32.lib comdlg32.lib winmm.lib opengl32.lib
cp "$B/Shoal.scr" "$B/Shoal.exe"
echo "built $B/Shoal.scr ($(stat -c %s "$B/Shoal.scr") bytes)"

if [ "${1:-}" = deploy ]; then
    WIN="${SHOAL_WIN:?set SHOAL_WIN to the Windows PC as an SSH target, e.g. SHOAL_WIN=user@windows-pc}"
    scp -q "$B/Shoal.scr" "$WIN:Shoal.scr"
    ssh "$WIN" 'copy /y "%USERPROFILE%\Shoal.scr" C:\Windows\System32\Shoal.scr >nul && echo installed C:\Windows\System32\Shoal.scr'
fi
