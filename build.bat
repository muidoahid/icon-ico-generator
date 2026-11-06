@echo off
REM icon.exe derleme betiği. Gereken: zig (https://ziglang.org/download/) PATH'te.
REM Alternatif: MSVC ile  cl /std:c++17 /O2 /utf-8 /EHsc /I third_party src\icon.cpp /link /SUBSYSTEM:WINDOWS shell32.lib user32.lib
cd /d "%~dp0"
where zig >nul 2>&1 || (echo HATA: zig PATH'te bulunamadi. https://ziglang.org/download/ & exit /b 1)
zig c++ -std=c++17 -O2 -s -Ithird_party src\icon.cpp -target x86_64-windows-gnu -Wl,--subsystem,windows -lshell32 -luser32 -o icon.exe
if errorlevel 1 (echo DERLEME BASARISIZ & exit /b 1)
echo Tamam: icon.exe
