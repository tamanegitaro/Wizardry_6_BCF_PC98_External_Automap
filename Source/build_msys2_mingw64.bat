@echo off
setlocal
cd /d "%~dp0"

set "GXX="
set "WINDRES="
set "OBJDUMP="
set "DEPENDENCY_REPORT=Wizardry6Automap_objdump.txt"
set "RUNTIME_REPORT=Wizardry6Automap_runtime_dlls.txt"

if exist C:\msys64\mingw64\bin\g++.exe set "GXX=C:\msys64\mingw64\bin\g++.exe"
if exist C:\msys64\mingw64\bin\windres.exe set "WINDRES=C:\msys64\mingw64\bin\windres.exe"
if exist C:\msys64\mingw64\bin\objdump.exe set "OBJDUMP=C:\msys64\mingw64\bin\objdump.exe"

if "%GXX%"=="" (
  where g++ >nul 2>nul
  if not errorlevel 1 set "GXX=g++"
)
if "%WINDRES%"=="" (
  where windres >nul 2>nul
  if not errorlevel 1 set "WINDRES=windres"
)
if "%OBJDUMP%"=="" (
  where objdump >nul 2>nul
  if not errorlevel 1 set "OBJDUMP=objdump"
)

if "%GXX%"=="" (
  echo g++.exe not found.
  echo Install MSYS2 MinGW64 and run:
  echo   pacman -S mingw-w64-x86_64-gcc
  pause
  exit /b 1
)
if "%WINDRES%"=="" (
  echo windres.exe not found.
  echo Install MSYS2 MinGW64 and run:
  echo   pacman -S mingw-w64-x86_64-binutils
  pause
  exit /b 1
)

if exist Wizardry6Automap.exe del /q Wizardry6Automap.exe
if exist Wizardry6Automap.res.o del /q Wizardry6Automap.res.o
if exist "%DEPENDENCY_REPORT%" del /q "%DEPENDENCY_REPORT%"
if exist "%RUNTIME_REPORT%" del /q "%RUNTIME_REPORT%"

echo Building PC-98 Wizardry 6 Automap V1.0 with %GXX%
"%WINDRES%" Wizardry6Automap.rc -O coff -o Wizardry6Automap.res.o
if errorlevel 1 (
  echo Resource build failed.
  pause
  exit /b 1
)

"%GXX%" -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror ^
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN ^
  -municode -mwindows ^
  -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
  -static -static-libgcc -static-libstdc++ ^
  -ffunction-sections -fdata-sections ^
  Wizardry6Automap.cpp Wizardry6Automap.res.o ^
  -o Wizardry6Automap.exe ^
  -Wl,--gc-sections ^
  -luser32 -lgdi32 -lcomdlg32 ^
  -s
if errorlevel 1 (
  echo Build failed.
  pause
  exit /b 1
)

if exist Wizardry6Automap.res.o del /q Wizardry6Automap.res.o

echo Built Wizardry6Automap.exe with warnings treated as errors.

if not "%OBJDUMP%"=="" (
  "%OBJDUMP%" -p Wizardry6Automap.exe > "%DEPENDENCY_REPORT%" 2>&1
  if errorlevel 1 (
    echo ERROR: objdump could not inspect Wizardry6Automap.exe.
    type "%DEPENDENCY_REPORT%"
    del /q "%DEPENDENCY_REPORT%" >nul 2>nul
    pause
    exit /b 1
  )

  findstr /I /C:"libgcc_s_" /C:"libstdc++-6.dll" /C:"libwinpthread-1.dll" "%DEPENDENCY_REPORT%" > "%RUNTIME_REPORT%"
  if not errorlevel 1 (
    echo ERROR: A MinGW runtime DLL dependency remains:
    type "%RUNTIME_REPORT%"
    echo.
    echo Imported DLLs reported by objdump:
    findstr /I /C:"DLL Name:" "%DEPENDENCY_REPORT%"
    del /q "%DEPENDENCY_REPORT%" >nul 2>nul
    del /q "%RUNTIME_REPORT%" >nul 2>nul
    pause
    exit /b 1
  )

  echo Imported DLLs reported by objdump:
  findstr /I /C:"DLL Name:" "%DEPENDENCY_REPORT%"
  echo Verified: no MinGW runtime DLL dependency detected.
  del /q "%DEPENDENCY_REPORT%" >nul 2>nul
  del /q "%RUNTIME_REPORT%" >nul 2>nul
) else (
  echo Note: objdump was not found, so runtime DLL dependencies were not automatically verified.
)

echo Administrator privileges are embedded in the EXE manifest.
echo Double-click Wizardry6Automap.exe to launch it.
pause
