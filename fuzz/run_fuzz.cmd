@echo off
rem Windows fuzz launcher: run from anywhere; extra arguments go to fuzz_win.py.
rem   fuzz\run_fuzz.cmd --minutes 40 --tag win
rem Defaults: app <repo>\build-asan\src\RelWithDebInfo\bambu-studio.exe, relay <repo>\build\src\Release\print_bot-mcp.exe,
rem output <repo>\temp\fuzz (override with --app / --mcp / --work or PRINTBOT_FUZZ_WORK).
setlocal
set "FZ=%~dp0"
if "%PRINTBOT_FUZZ_WORK%"=="" set "PRINTBOT_FUZZ_WORK=%FZ%..\temp\fuzz"
if not exist "%PRINTBOT_FUZZ_WORK%\corpus" python "%FZ%gen_corpus.py" || exit /b 1
python "%FZ%fuzz_win.py" %*
endlocal
