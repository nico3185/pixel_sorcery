# install.ps1 - one-time setup of the Windows build environment (v0.1.1): MSYS2 with the MinGW64
# toolchain the engine is built and tested with in CI (gcc, cmake, ninja, SDL2, zlib, Python with
# capstone and curses), plus git and zsh for the build scripts. Downloads from MSYS2's own mirrors,
# about 600 MB the first time. Then double-click PixelSorcery.cmd (the build screen: put your own
# ROMs in, build, play). No game data is included or downloaded.
#   powershell -ExecutionPolicy Bypass -File engine\tools\windows\install.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$msys = 'C:\msys64'
if (-not (Test-Path "$msys\usr\bin\bash.exe")) {
    Write-Host 'Installing MSYS2 (winget)...'
    winget install --id MSYS2.MSYS2 -e --silent --accept-package-agreements --accept-source-agreements
}
$pkgs = 'git zsh mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 ' +
        'mingw-w64-x86_64-zlib mingw-w64-x86_64-python mingw-w64-x86_64-python-capstone'
$env:MSYSTEM = 'MINGW64'
Write-Host 'Updating MSYS2 and installing the toolchain...'
& "$msys\usr\bin\bash.exe" -lc 'pacman -Syu --noconfirm'
& "$msys\usr\bin\bash.exe" -lc "pacman -S --noconfirm --needed $pkgs"
# the build scripts call engine/.venv/bin/python: MinGW's Python (capstone, curses)
$venv = Join-Path $root 'engine\.venv\bin'
New-Item -ItemType Directory -Force $venv | Out-Null
Set-Content -Force -Encoding ascii -NoNewline -Path (Join-Path $venv 'python') -Value "#!/bin/sh`nexec /mingw64/bin/python `"`$@`"`n"
& "$msys\usr\bin\bash.exe" -lc "gcc --version | head -1; python -c 'import capstone, curses; print(\""python ok\"")'"
Write-Host "Done. Start PixelSorcery.cmd in $root"
