@echo off
setlocal
pushd "%~dp0"

call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
mkdir ..\build 2>nul
rc /nologo /fo "..\build\Shoal.res" Shoal.rc
if errorlevel 1 (
    echo Build failed.
    popd
    exit /b 1
)
cl /nologo /EHsc /DUNICODE /D_UNICODE /O2 /std:c++17 /Fo"..\build\Main.obj" /Fe"..\build\ShoalNative.exe" Main.cpp "..\build\Shoal.res" user32.lib gdi32.lib kernel32.lib advapi32.lib

if errorlevel 1 (
    echo Build failed.
    popd
    exit /b 1
) else if exist "..\build\ShoalNative.exe" (
    rem The same binary as a screensaver; copy Shoal.scr to C:\Windows\System32 to list it in Screen Saver Settings.
    copy /y "..\build\ShoalNative.exe" "..\build\Shoal.scr" >nul
    echo Build succeeded: ..\build\ShoalNative.exe and ..\build\Shoal.scr
) else (
    echo Build failed.
    popd
    exit /b 1
)

popd
