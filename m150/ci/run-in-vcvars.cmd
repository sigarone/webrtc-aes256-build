@echo off
rem run-in-vcvars.cmd - run a command inside the runner Visual Studio x64 environment
rem (INCLUDE / LIB / PATH as the MSVC + Windows SDK of the image define them).
rem usage: run-in-vcvars.cmd <program> [args...]
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
%*
exit /b %ERRORLEVEL%
