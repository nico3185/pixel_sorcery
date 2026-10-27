<#
  genrecomp setup (Windows). Run through Setup.cmd.

  Runs exactly the steps in README "Step by step", skipping any already done,
  so a rerun after a failure resumes. Asks before installing anything.
  Details go to setup.log in the repo folder; the console gets one line per step.

  -Yes   answer yes to every install prompt (unattended)
  -Rom   path to a ROM for the optional smoke test
#>
param([switch]$Yes, [string]$Rom = "")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Log = Join-Path $Root "setup.log"
$Build = Join-Path $Root "build"
$GpgxUrl = "https://github.com/ekeeke/Genesis-Plus-GX.git"
$GpgxCommit = "c3df2d2"   # the submodule pin; keep in step with ext/Genesis-Plus-GX
"genrecomp setup $(Get-Date -Format s)" | Out-File $Log -Encoding utf8

function Say($msg) { Write-Host $msg; $msg | Out-File $Log -Append -Encoding utf8 }
function Fail($msg) {
    Write-Host ""
    Write-Host "Setup stopped: $msg" -ForegroundColor Red
    Write-Host "Details are in $Log"
    exit 1
}
function Run($what, $exe, $argList) {
    "`n> $exe $argList" | Out-File $Log -Append -Encoding utf8
    $p = Start-Process -FilePath $exe -ArgumentList $argList -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$Log.out" -RedirectStandardError "$Log.err"
    Get-Content "$Log.out", "$Log.err" -ErrorAction SilentlyContinue | Out-File $Log -Append -Encoding utf8
    Remove-Item "$Log.out", "$Log.err" -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { Fail "$what failed (exit $($p.ExitCode))." }
}
function Ask($question) {
    if ($Yes) { return $true }
    return (Read-Host "$question [y/N]") -match '^[yY]'
}
function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }
function Import-VCVars($vsPath) {
    # Outside a "Developer Command Prompt", PATH/INCLUDE/LIB (and rc.exe's folder)
    # aren't set, which breaks vcpkg/cmake builds with "rc ... no such file or directory".
    $vcvarsall = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvarsall)) { Fail "vcvarsall.bat not found under $vsPath." }
    $vars = cmd /c "`"$vcvarsall`" x64 >nul 2>&1 && set"
    foreach ($line in $vars) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process") }
    }
}

# 1. Prerequisites ---------------------------------------------------------
Say "1/5 Checking prerequisites"
if (-not (Have git)) {
    if (Ask "Git is missing. Install it with winget (about 60 MB)?") {
        Run "Installing Git" "winget" "install --id Git.Git -e --silent --accept-package-agreements --accept-source-agreements"
        Fail "Git was installed. Close this window and run Setup.cmd again so the new PATH is picked up."
    } else { Fail "Git is required. Install it from https://git-scm.com and run Setup.cmd again." }
}
if (-not (Have cmake)) {
    if (Ask "CMake is missing. Install it with winget (about 40 MB)?") {
        Run "Installing CMake" "winget" "install --id Kitware.CMake -e --silent --accept-package-agreements --accept-source-agreements"
        Fail "CMake was installed. Close this window and run Setup.cmd again so the new PATH is picked up."
    } else { Fail "CMake 3.16 or newer is required. Install it from https://cmake.org and run Setup.cmd again." }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs) {
    if (Ask "Visual Studio Build Tools (C++ workload) is missing. Install it with winget (a few GB, several minutes)?") {
        Run "Installing Visual Studio Build Tools" "winget" "install --id Microsoft.VisualStudio.2022.BuildTools -e --silent --accept-package-agreements --accept-source-agreements --override `"--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`""
        $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
        if (-not $vs) { Fail "Visual Studio Build Tools install finished but the C++ workload was not detected. Check setup.log." }
    } else { Fail "Visual Studio 2022 with 'Desktop development with C++' is required (free Community edition: https://visualstudio.microsoft.com), or run: winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override `"--add Microsoft.VisualStudio.Workload.VCTools`"" }
}
Say "    git, cmake and Visual Studio found"
Import-VCVars $vs
Say "    VC environment (PATH/INCLUDE/LIB, rc.exe) imported from vcvarsall.bat"

# vcpkg + SDL2 + zlib
$Vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\vcpkg" }
if (-not (Test-Path "$Vcpkg\vcpkg.exe")) {
    if (Ask "vcpkg (the C library manager, used for SDL2/zlib) is not at $Vcpkg. Install it there (about 100 MB)?") {
        Run "Downloading vcpkg" "git" "clone https://github.com/microsoft/vcpkg `"$Vcpkg`""
        Run "Bootstrapping vcpkg" "$Vcpkg\bootstrap-vcpkg.bat" "-disableMetrics"
    } else { Fail "SDL2 and zlib come from vcpkg. Set VCPKG_ROOT to an existing vcpkg, or allow the install." }
}
if (-not (Test-Path "$Vcpkg\installed\x64-windows\share\sdl2")) {
    if (Ask "SDL2 is not installed in vcpkg. Build it now (a few minutes, about 50 MB)?") {
        Run "Installing SDL2" "$Vcpkg\vcpkg.exe" "install sdl2:x64-windows"
    } else { Fail "SDL2 is required: run '$Vcpkg\vcpkg.exe install sdl2:x64-windows' and then Setup.cmd again." }
}
if (-not (Test-Path "$Vcpkg\installed\x64-windows\share\zlib")) {
    if (Ask "zlib is not installed in vcpkg. Build it now (a few seconds)?") {
        Run "Installing zlib" "$Vcpkg\vcpkg.exe" "install zlib:x64-windows"
    } else { Fail "zlib is required: run '$Vcpkg\vcpkg.exe install zlib:x64-windows' and then Setup.cmd again." }
}
Say "    SDL2 and zlib found in $Vcpkg"

# 2. Genesis Plus GX -------------------------------------------------------
Say "2/5 Fetching Genesis Plus GX"
$gpgx = Join-Path $Root "ext\Genesis-Plus-GX"
if (-not (Test-Path "$gpgx\core\system.c")) {
    if (Test-Path (Join-Path $Root ".git")) {
        Run "Fetching the Genesis Plus GX submodule" "git" "-C `"$Root`" submodule update --init"
    } else {
        # Plain zip download: GitHub zips don't include submodules
        if (Test-Path $gpgx) { Remove-Item $gpgx -Recurse -Force }
        Run "Downloading Genesis Plus GX" "git" "clone $GpgxUrl `"$gpgx`""
        Run "Pinning Genesis Plus GX" "git" "-C `"$gpgx`" checkout $GpgxCommit"
    }
}
Say "    ext\Genesis-Plus-GX ready"

# 3. Configure and build ---------------------------------------------------
Say "3/5 Building (first build takes a minute or two)"
if (-not (Test-Path "$Build\CMakeCache.txt")) {
    Run "Configuring" "cmake" "-S `"$Root`" -B `"$Build`" -A x64 -DCMAKE_TOOLCHAIN_FILE=`"$Vcpkg\scripts\buildsystems\vcpkg.cmake`""
}
Run "Building" "cmake" "--build `"$Build`" --config Release"
Say "    built build\Release\genrecomp_ref.exe"

# 4. Self-check ------------------------------------------------------------
Say "4/5 Running the self-check"
Run "The self-check (ctest)" "ctest" "--test-dir `"$Build`" -C Release --output-on-failure"
Say "    test_runtime: ok"

# 5. Optional smoke test with the user's own ROM + launcher ----------------
Say "5/5 Launcher"
$launcher = Join-Path $Root "Reference Runner.cmd"
@"
@echo off
rem Plays a Genesis ROM on the reference runner. Drag a ROM onto this file.
"%~dp0build\Release\genrecomp_ref.exe" %*
"@ | Out-File $launcher -Encoding ascii
Say "    created 'Reference Runner.cmd' (drag a ROM onto it)"

if (-not $Rom -and -not $Yes) {
    $Rom = (Read-Host "Optional: drag your own Genesis ROM here and press Enter to smoke-test it (Enter to skip)").Trim('"', ' ')
}
if ($Rom) {
    if (-not (Test-Path $Rom)) { Fail "No file at $Rom." }
    Run "The smoke test" "$Build\Release\genrecomp_ref.exe" "--headless --frames 600 `"$Rom`""
    Say "    your ROM ran 600 frames headless"
}

Write-Host ""
Write-Host "Done. genrecomp is built in $Build." -ForegroundColor Green
Write-Host "Next: README 'Usage', or a title that uses genrecomp (e.g. sp00nznet/pigskin)."
