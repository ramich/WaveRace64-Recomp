@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\dev\src\github\WaveRace64-Recomp
lib\N64Recomp\build\N64Recomp.exe recomp\waverace64.toml > recomp.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build --config Release -j 12 > build.log 2>&1
exit /b %errorlevel%
