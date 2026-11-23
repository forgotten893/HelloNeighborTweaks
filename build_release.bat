@echo off
rem Builds the release files in dist\:
rem   HelloNeighborTweaks-<ver>-Installer.zip  Setup.exe     + README + LICENSE + CHECKSUMS + BUILD-INFO + source\ (DLL + installer)
rem   HelloNeighborTweaks-<ver>-Portable.zip   XINPUT1_3.dll + README + LICENSE + CHECKSUMS + BUILD-INFO + source\ (DLL only)
rem   SHA256SUMS.txt                           SHA-256 of both zips and of the DLL and Setup.exe inside them
rem The portable zip has nothing of the installer: its readme, checksums, build info and source are DLL-only.
setlocal
cd /d "%~dp0"
for /f "tokens=3" %%v in ('findstr /c:"#define HNT_VERSION " src\version.h') do set "VER=%%~v"
if not defined VER (echo Could not read the version from src\version.h & exit /b 1)
call "%~dp0build.bat" || exit /b 1
call :vcvars || exit /b 1
if exist dist rmdir /s /q dist
mkdir dist\obj dist\installer\source\src dist\installer\source\installer dist\installer\source\tools dist\portable\source\src || exit /b 1
set "PS=powershell -NoProfile -ExecutionPolicy Bypass"

rem --- readmes
%PS% -File tools\make_readme.ps1 -Variant installer -In README.md -Out dist\obj\README-installer.txt || exit /b 1
%PS% -File tools\make_readme.ps1 -Variant portable  -In README.md -Out dist\obj\README-portable.txt || exit /b 1

rem --- source code: everything for the installer zip, the DLL's part for the portable zip
for %%f in (src\ssr_unlock.c src\version.h src\version.rc) do (
    copy /y "%%f" dist\installer\source\src\ >nul || exit /b 1
    copy /y "%%f" dist\portable\source\src\ >nul || exit /b 1
)
for %%f in (installer\setup.c installer\setup.rc installer\resource.h installer\setup.manifest) do copy /y "%%f" dist\installer\source\installer\ >nul || exit /b 1
copy /y tools\make_readme.ps1 dist\installer\source\tools\ >nul || exit /b 1
for %%f in (build.bat build_release.bat README.md LICENSE .gitignore) do copy /y "%%f" dist\installer\source\ >nul || exit /b 1
for %%f in (build.bat LICENSE .gitignore) do copy /y "%%f" dist\portable\source\ >nul || exit /b 1
copy /y dist\obj\README-portable.txt dist\portable\source\README.md >nul || exit /b 1
tar -a -c -f dist\obj\source.zip -C dist\installer\source * || exit /b 1
copy /y build\XINPUT1_3.dll dist\obj\ >nul

rem --- installer exe (embeds the DLL above, the installer readme, the license and the installer zip's source)
rc /nologo /fo dist\obj\setup.res installer\setup.rc || exit /b 1
cl /nologo /O2 /MT /W4 /Brepro installer\setup.c dist\obj\setup.res /Fo:dist\obj\ /Fe:"dist\obj\HelloNeighborTweaks-%VER%-Setup.exe" /link /NOLOGO /SUBSYSTEM:WINDOWS /Brepro || exit /b 1

rem --- build info (the installer zip's copy has one more paragraph about Setup.exe)
set "INFO=dist\portable\BUILD-INFO.txt"
> "%INFO%" echo Hello Neighbor Tweaks %VER%
>>"%INFO%" echo.
>>"%INFO%" echo Built from the source code in source\ with build.bat, using:
for /f "delims=" %%l in ('cl 2^>^&1 ^| findstr /c:"Version"') do >>"%INFO%" echo   %%l
for /f "delims=" %%l in ('link 2^>^&1 ^| findstr /c:"Version"') do >>"%INFO%" echo   %%l
>>"%INFO%" echo.
>>"%INFO%" echo XINPUT1_3.dll is built with /Brepro (deterministic): building source\ with build.bat and the same
>>"%INFO%" echo compiler version gives a byte-identical file, so you can check it against CHECKSUMS.txt.
copy /y "%INFO%" dist\installer\BUILD-INFO.txt >nul
>>dist\installer\BUILD-INFO.txt echo.
>>dist\installer\BUILD-INFO.txt echo HelloNeighborTweaks-%VER%-Setup.exe is built from the same source with build_release.bat. It contains
>>dist\installer\BUILD-INFO.txt echo exactly this XINPUT1_3.dll and copies it into the game folder unchanged: the installed file
>>dist\installer\BUILD-INFO.txt echo has the SHA-256 listed in CHECKSUMS.txt.

rem --- checksums of the binaries in each zip
%PS% -Command "function W($o, $files) { 'SHA-256 of the files in this download (check: Get-FileHash <file> in PowerShell, or certutil -hashfile <file> SHA256)' | Set-Content $o -Encoding ascii; '' | Add-Content $o -Encoding ascii; foreach ($f in $files) { '{0}  {1}' -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLower(), (Split-Path $f -Leaf) | Add-Content $o -Encoding ascii } }; W 'dist\installer\CHECKSUMS.txt' @('dist\obj\HelloNeighborTweaks-%VER%-Setup.exe','dist\obj\XINPUT1_3.dll'); W 'dist\portable\CHECKSUMS.txt' @('dist\obj\XINPUT1_3.dll')" || exit /b 1

rem --- the two zips
copy /y dist\obj\README-installer.txt dist\installer\README.txt >nul
copy /y dist\obj\README-portable.txt dist\portable\README.txt >nul
copy /y LICENSE dist\installer\LICENSE.txt >nul
copy /y LICENSE dist\portable\LICENSE.txt >nul
copy /y "dist\obj\HelloNeighborTweaks-%VER%-Setup.exe" dist\installer\ >nul
copy /y dist\obj\XINPUT1_3.dll dist\portable\ >nul
tar -a -c -f "dist\HelloNeighborTweaks-%VER%-Installer.zip" -C dist\installer * || exit /b 1
tar -a -c -f "dist\HelloNeighborTweaks-%VER%-Portable.zip" -C dist\portable * || exit /b 1
%PS% -Command "$o='dist\SHA256SUMS.txt'; foreach ($f in 'dist\HelloNeighborTweaks-%VER%-Installer.zip','dist\HelloNeighborTweaks-%VER%-Portable.zip','dist\obj\HelloNeighborTweaks-%VER%-Setup.exe','dist\obj\XINPUT1_3.dll') { '{0}  {1}' -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLower(), (Split-Path $f -Leaf) | Add-Content $o -Encoding ascii }" || exit /b 1

rmdir /s /q dist\installer dist\portable
echo.
echo Release %VER%:
dir /b dist\*.zip dist\*.txt
type dist\SHA256SUMS.txt
exit /b 0

:vcvars
where cl >nul 2>nul && exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with the x64 C++ tools found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
