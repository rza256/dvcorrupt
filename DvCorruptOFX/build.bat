@echo off
rem Builds DvCorrupt.ofx (OpenFX plugin for VEGAS Pro) and the bundle folder.
setlocal
cd /d "%~dp0"
call "D:\VS2022\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
set INC=/I external\openfx\include /I src
set CFLAGS=/nologo /O2 /EHsc /std:c++17 /MT /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS

cl %CFLAGS% %INC% /LD src\DvCorruptOFX.cpp /Fo:build\ /Fe:build\DvCorrupt.dll /link /DLL || exit /b 1
cl %CFLAGS% %INC% tests\ofxtest.cpp /Fo:build\ /Fe:build\ofxtest.exe || exit /b 1

set B=build\DvCorrupt.ofx.bundle\Contents
if exist build\DvCorrupt.ofx.bundle rmdir /s /q build\DvCorrupt.ofx.bundle
mkdir %B%\Win64 %B%\Presets %B%\Resources
copy /y build\DvCorrupt.dll %B%\Win64\DvCorrupt.ofx >nul
build\ofxtest.exe presets %B%\Presets\PresetPackage.xml || exit /b 1
echo Built build\DvCorrupt.ofx.bundle
