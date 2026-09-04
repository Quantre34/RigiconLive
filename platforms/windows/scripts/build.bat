@echo off
setlocal enableextensions

REM Rigicon Live - Windows build script.
REM Tries MinGW-w64 (gcc + windres) first, then MSVC (cl + rc).
REM Output: dist\windows\RigiconLive.exe

pushd "%~dp0..\..\.."

if not exist dist\windows mkdir dist\windows
if not exist build         mkdir build

set SRC=src\main.c src\crypto.c src\net.c src\term.c src\notify.c

REM ---- MinGW-w64 (preferred: produces a smaller, dependency-free static exe)
where gcc >nul 2>&1
if %ERRORLEVEL%==0 goto :mingw

REM ---- MSVC
where cl >nul 2>&1
if %ERRORLEVEL%==0 goto :msvc

echo.
echo [!] Ne gcc (MinGW-w64) ne de cl (MSVC) PATH icinde.
echo     MSYS2/MinGW-w64 kur veya "x64 Native Tools Command Prompt" ac.
popd
exit /b 1

:mingw
echo [*] MinGW-w64 tespit edildi.
windres build\resource.rc -O coff -o build\resource.res
if errorlevel 1 goto :err
gcc -O2 -Wall -Wextra -std=c99 ^
    %SRC% build\resource.res ^
    -o dist\windows\RigiconLive.exe ^
    -lws2_32 -liphlpapi -lbcrypt -static -static-libgcc
if errorlevel 1 goto :err
goto :done

:msvc
echo [*] MSVC tespit edildi.
rc /nologo /fo build\resource.res build\resource.rc
if errorlevel 1 goto :err
cl /nologo /O2 /W3 /MT /utf-8 ^
    /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 ^
    /Fedist\windows\RigiconLive.exe /Fobuild\ ^
    %SRC% build\resource.res ^
    /link ws2_32.lib iphlpapi.lib bcrypt.lib
if errorlevel 1 goto :err
goto :done

:err
echo.
echo [!] Derleme basarisiz.
popd
exit /b 1

:done
echo.
echo [+] Basarili: dist\windows\RigiconLive.exe
echo.
echo Sonraki adimlar:
echo   1. platforms\windows\scripts\generate-cert.ps1    (bir kereye mahsus)
echo   2. platforms\windows\scripts\sign-exe.ps1
popd
endlocal
