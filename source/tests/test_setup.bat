@echo off
rem Run after build.bat. Tests setup using a private application hive, temp
rem known folders and a cache callback. Never installs into the active account.
setlocal EnableExtensions DisableDelayedExpansion
set "SOURCE=%~dp0.."
if not exist "%SOURCE%\obj\Setup.res" goto needbuild
if not exist "%SOURCE%\bin\ZMSviewer-Setup.exe" goto needbuild
if not exist "%SOURCE%\bin\ZMSviewer.dll" goto needbuild

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "TESTBUILD=%TEMP%\ZMSviewer-Test-%RANDOM%-%RANDOM%-%RANDOM%"
mkdir "%TESTBUILD%" || exit /b 1
pushd "%TESTBUILD%" || exit /b 1
cl /nologo /O2 /std:c++17 /EHsc /MT /W4 /utf-8 /DUNICODE /D_UNICODE "%SOURCE%\tests\test_setup.cpp" "%SOURCE%\obj\Setup.res" /Fe:test_setup.exe /link /SUBSYSTEM:CONSOLE advapi32.lib shell32.lib ole32.lib comctl32.lib user32.lib gdi32.lib msimg32.lib shlwapi.lib uuid.lib windowscodecs.lib
if errorlevel 1 goto done
test_setup.exe "%SOURCE%\bin\ZMSviewer-Setup.exe" "%SOURCE%\bin\ZMSviewer.dll"
:done
set "RESULT=%ERRORLEVEL%"
popd
del /q "%TESTBUILD%\test_setup.obj" "%TESTBUILD%\test_setup.exe" 2>nul
rmdir "%TESTBUILD%" 2>nul
exit /b %RESULT%
:needbuild
echo Run build.bat before testing the installer.
exit /b 1
