@echo off
rem Builds ZMSviewer (x64, Release) with the Visual Studio 2022 tools:
rem   bin\ZMSviewer.dll        the thumbnail providers
rem   bin\ThumbTest.exe        test tool
rem   bin\ZMSviewer-Setup.exe  the installer, with the DLL inside
rem "build.bat nosetup" stops after the DLL and the test tool.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
rem Delayed expansion keeps the ")" of "(x86)" from closing the for loop.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist bin mkdir bin
if not exist obj mkdir obj

set "DEFS=/DUNICODE /D_UNICODE /DBCDEC_BC4BC5_PRECISE /DTINYEXR_USE_MINIZ=0 /DTINYEXR_USE_STB_ZLIB=1 /DTINYEXR_USE_THREAD=0 /DTINYEXR_USE_OPENMP=0"
set "FLAGS=/nologo /O2 /EHsc /MT /GS /guard:cf /utf-8"

rem ufbx is C.
cl %FLAGS% /W0 /c /TC vendor\ufbx.c /Foobj\ufbx.obj || exit /b 1
cl %FLAGS% /W0 /c /TC vendor\zstddeclib.c /Foobj\zstddeclib.obj || exit /b 1
cl %FLAGS% /std:c++17 /W3 /c %DEFS% src\Vendor.cpp /Foobj\Vendor.obj || exit /b 1
cl %FLAGS% /std:c++17 /W4 /wd4244 /wd4267 /wd4100 /c %DEFS% src\Common.cpp src\Psd.cpp src\Images.cpp src\Books.cpp src\Pdf.cpp src\Render.cpp src\Models.cpp src\Provider.cpp /Foobj\ || exit /b 1
rc /nologo /d ZMSVIEWER_PROVIDER /fo obj\ProviderVersion.res src\Setup.rc || exit /b 1
link /nologo /DLL /OUT:bin\ZMSviewer.dll /DEF:src\ZMSviewer.def /guard:cf /DYNAMICBASE /NXCOMPAT obj\ProviderVersion.res ^
  obj\ufbx.obj obj\zstddeclib.obj obj\Vendor.obj obj\Common.obj obj\Psd.obj obj\Images.obj obj\Books.obj obj\Pdf.obj obj\Render.obj obj\Models.obj obj\Provider.obj ^
  ole32.lib gdi32.lib shlwapi.lib windowscodecs.lib || exit /b 1

cl %FLAGS% /std:c++17 /W4 /DUNICODE /D_UNICODE src\ThumbTest.cpp /Foobj\ /Fe:bin\ThumbTest.exe /link gdi32.lib || exit /b 1
if /i "%~1"=="nosetup" exit /b 0

rc /nologo /fo obj\Setup.res src\Setup.rc || exit /b 1
cl %FLAGS% /std:c++17 /W4 /DUNICODE /D_UNICODE src\Setup.cpp obj\Setup.res /Foobj\ /Fe:bin\ZMSviewer-Setup.exe /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT ^
  advapi32.lib shell32.lib ole32.lib comctl32.lib user32.lib gdi32.lib msimg32.lib shlwapi.lib uuid.lib windowscodecs.lib || exit /b 1
echo Built bin\ZMSviewer.dll, bin\ThumbTest.exe and bin\ZMSviewer-Setup.exe
