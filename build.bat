@echo off
rem Builds fastdu.exe with Visual Studio's C compiler (cl). Run it from an
rem "x64 Native Tools Command Prompt"; "build.bat 1.2.0" stamps a version.
setlocal
set VERSION_FLAG=
if not "%~1"=="" set VERSION_FLAG=/DFASTDU_VERSION=%~1
cl /nologo /O2 /W3 %VERSION_FLAG% /Fe:fastdu.exe /Fo:fastdu.obj src\windows\fastdu.c
