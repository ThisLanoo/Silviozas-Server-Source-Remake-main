@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM --- locate Visual Studio (vcvars64) ---
set "VCVARS="
set "P1=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "P2=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
set "P3=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
set "P4=%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "P5=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "!P1!" set "VCVARS=!P1!"
if not defined VCVARS if exist "!P2!" set "VCVARS=!P2!"
if not defined VCVARS if exist "!P3!" set "VCVARS=!P3!"
if not defined VCVARS if exist "!P4!" set "VCVARS=!P4!"
if not defined VCVARS if exist "!P5!" set "VCVARS=!P5!"
if not defined VCVARS (
  echo Could not find vcvars64.bat - edit build.bat with your Visual Studio path.
  exit /b 1
)
call "!VCVARS!" >nul

if not exist build mkdir build
cd build

echo === Compiling ENet ===
cl /nologo /c /O2 /MD /DWIN32 /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS ^
  /I "..\enet-master\include" ^
  ..\enet-master\address.c ..\enet-master\callbacks.c ..\enet-master\compress.c ^
  ..\enet-master\host.c ..\enet-master\list.c ..\enet-master\packet.c ^
  ..\enet-master\peer.c ..\enet-master\protocol.c ..\enet-master\win32.c
if errorlevel 1 (echo ENET COMPILE FAILED & exit /b 1)

echo === Compiling server ===
cl /nologo /c /O2 /MD /EHsc /std:c++17 /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS ^
  /I "..\enet-master\include" /I "..\connectors" ^
  ..\silviozas-server.cpp ..\connectors\utils.cpp
if errorlevel 1 (echo CPP COMPILE FAILED & exit /b 1)

echo === Linking ===
link /nologo /OUT:silviozas-server.exe *.obj ws2_32.lib winmm.lib user32.lib
if errorlevel 1 (echo LINK FAILED & exit /b 1)

echo === BUILD OK -^> build\silviozas-server.exe ===
copy /y "..\serverdata.txt" serverdata.txt >nul 2>&1
copy /y "..\send.txt" send.txt >nul 2>&1
