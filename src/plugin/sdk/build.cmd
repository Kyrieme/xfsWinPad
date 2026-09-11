@echo off
rem xfsWinPad AI workshop - fixed build pipeline. DO NOT MODIFY.
rem Locates VS (vswhere), sets x64 environment, compiles src\*.c to build\output.dll
setlocal enableextensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [workshop] vswhere.exe not found - install Visual Studio 2022+ with C++ tools >&2
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo [workshop] no VS with C++ workload found >&2
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" -no_logo >nul 2>&1
if errorlevel 1 (
  echo [workshop] vcvars64 failed >&2
  exit /b 1
)

if not exist build mkdir build
cl /nologo /W3 /O2 /LD /utf-8 /TC ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /I "__SDK_INCLUDE_DIR__" ^
   src\*.c ^
   /Fe:build\output.dll ^
   /link /DLL /NOLOGO /SUBSYSTEM:WINDOWS user32.lib shell32.lib shlwapi.lib
if errorlevel 1 (
  echo [workshop] compile failed >&2
  exit /b 1
)
if not exist build\output.dll (
  echo [workshop] output.dll missing >&2
  exit /b 1
)
echo [workshop] build ok: build\output.dll
exit /b 0

