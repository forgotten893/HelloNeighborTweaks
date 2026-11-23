@echo off
rem Builds build\XINPUT1_3.dll only (x64, static CRT, no external dependencies).
rem Add /DHNT_DEBUG_HOTKEYS=1 to the cl line for the developer test hotkeys (F5-F11; off in releases).
rem Needs Visual Studio 2019 or later (or the Build Tools) with the "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"
call :vcvars || exit /b 1
if not exist build mkdir build
rc /nologo /fo build\version.res src\version.rc || exit /b 1
rem /Brepro = deterministic output (no timestamps): the same source + compiler version gives a byte-identical DLL
cl /nologo /O2 /MT /W4 /wd4201 /LD /Brepro src\ssr_unlock.c build\version.res /Fo:build\ /Fe:build\XINPUT1_3.dll /link /NOLOGO /DLL /Brepro || exit /b 1
del build\*.obj build\*.exp build\*.lib build\*.res 2>nul
echo Built %~dp0build\XINPUT1_3.dll
exit /b 0

:vcvars
where cl >nul 2>nul && exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - install Visual Studio or the Build Tools with C++. & exit /b 1)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with the x64 C++ tools found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
