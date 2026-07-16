@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\dev\src\github\WaveRace64-Recomp
lib\N64Recomp\build\N64Recomp.exe recomp\waverace64.toml > recomp.log 2>&1
if errorlevel 1 (echo REBUILD FAILED: N64Recomp step, see recomp.log & exit /b 1)
rem NOTE (2026-07-16): invoke this script from PowerShell or Python
rem (subprocess ["cmd","/c",...]), NOT from Git Bash: MSYS argument
rem conversion can turn `cmd /c` into `cmd C:\` so the batch silently never
rem runs (banner only, exit 0, stale exe). From bash use `cmd //c` if you
rem must. Also keep this file ASCII with CRLF line endings - cmd misparses
rem LF-only batch files silently.
rem Ninja is invoked directly (not `cmake --build`, which was unreliable
rem here) and twice: the N64Recomp step regenerates RecompiledFuncs which
rem triggers a CMake reconfigure inside the first ninja run; the second run
rem is a cheap no-op safety net that picks up anything the reconfigure pass
rem missed.
cd build
ninja > ..\build.log 2>&1
if errorlevel 1 (cd .. & echo REBUILD FAILED: ninja pass 1, see build.log & exit /b 1)
ninja > ..\build2.log 2>&1
if errorlevel 1 (cd .. & echo REBUILD FAILED: ninja pass 2, see build2.log & exit /b 1)
cd ..
rem Staleness guard: fail loudly if any source file is still newer than the
rem exe after both ninja passes (the class of silent-stale-build failures
rem that burned this project on 2026-07-16 - see docs/RE-NOTES.md).
powershell -NoProfile -Command "$exe = Get-Item build\WaveRace64Recomp.exe; $stale = Get-ChildItem src -Recurse -Include *.cpp,*.h | Where-Object { $_.LastWriteTime -gt $exe.LastWriteTime }; if ($stale) { $stale | ForEach-Object { Write-Host ('REBUILD FAILED: exe stale vs ' + $_.FullName) }; exit 1 } else { Write-Host ('rebuild OK: ' + $exe.LastWriteTime) }"
exit /b %errorlevel%
