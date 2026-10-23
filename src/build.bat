@echo off
rem Builds the 32-bit NewLook.asi into ..\build
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\build mkdir ..\build
cl /nologo /O2 /MT /W3 /EHsc /arch:SSE2 /D_CRT_SECURE_NO_WARNINGS /LD newlook.cpp /Fo..\build\ /Fe..\build\NewLook.asi || exit /b 1
del ..\build\NewLook.exp ..\build\NewLook.lib 2>nul
echo OK
