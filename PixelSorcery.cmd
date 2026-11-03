@echo off
rem PixelSorcery on Windows: the build screen (build_ui.py) in a MinGW64 terminal. First run
rem engine\tools\windows\install.ps1 once. Your ROMs: the build screen asks for their folder.
if not exist C:\msys64\msys2_shell.cmd (
  echo MSYS2 is missing: run  powershell -ExecutionPolicy Bypass -File engine\tools\windows\install.ps1
  pause & exit /b 1
)
C:\msys64\msys2_shell.cmd -mingw64 -defterm -no-start -here -c "zsh ./engine/tools/build_all.sh --tui"
