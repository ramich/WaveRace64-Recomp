@echo off
rem Repo root = this script's parent (scripts\..); no hardcoded machine path.
cd /d "%~dp0.."
rem Locate vcvars64.bat: honor %VCVARS%, else vswhere, else the default install.
if not defined VCVARS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find "VC\Auxiliary\Build\vcvars64.bat" 2^>nul`) do set "VCVARS=%%i"
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
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
rem On failure, distill the compiler error lines into build_errors.log so the
rem cause is one short Read away. IMPORTANT: some callers (e.g. the Claude Code
rem PowerShell tool) suppress a command's stdout when it exits non-zero, so the
rem "REBUILD FAILED" echo and error text may be invisible in the terminal - ALWAYS
rem read build_errors.log (short) or build.log (full) after a failed rebuild.
cd build
ninja > ..\build.log 2>&1
if errorlevel 1 (cd .. & findstr /i /n /c:"error" build.log > build_errors.log & echo REBUILD FAILED: ninja pass 1 - see build_errors.log ^(short^) or build.log ^(full^) & type build_errors.log & exit /b 1)
ninja > ..\build2.log 2>&1
if errorlevel 1 (cd .. & findstr /i /n /c:"error" build2.log > build_errors.log & echo REBUILD FAILED: ninja pass 2 - see build_errors.log ^(short^) or build2.log ^(full^) & type build_errors.log & exit /b 1)
cd ..
rem Staleness guard: fail loudly if any source file is still newer than the
rem exe after both ninja passes (the class of silent-stale-build failures
rem that burned this project on 2026-07-16 - see docs/RE-NOTES.md).
powershell -NoProfile -Command "$exe = Get-Item build\WaveRace64Recomp.exe; $stale = Get-ChildItem src -Recurse -Include *.cpp,*.h | Where-Object { $_.LastWriteTime -gt $exe.LastWriteTime }; if ($stale) { $stale | ForEach-Object { Write-Host ('REBUILD FAILED: exe stale vs ' + $_.FullName) }; exit 1 } else { Write-Host ('rebuild OK: ' + $exe.LastWriteTime) }"
exit /b %errorlevel%
