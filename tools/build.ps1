# Builds Gundam Operation Troy (PC recomp) on Windows from YOUR OWN copy of the game.
#
# Run it with build.bat in the repository root (double-click it). It will:
#   1. download the SDK's helper libraries (git submodules), first run only
#   2. find Visual Studio's C++ tools and clang
#   3. extract the game from your GOD folder or .iso into game\ (first run only)
#   4. convert the game's code and compile everything
#
# Everything it writes stays on this PC. game\, generated\ and out\ are ignored by git.
# A full log is written to out\build.log.

param(
    [string]$GamePath = "",
    [ValidateSet("release", "relwithdebinfo", "debug")]
    [string]$Config = "release"
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
New-Item -ItemType Directory -Force -Path (Join-Path $Root "out") | Out-Null
$Log = Join-Path $Root "out\build.log"
[System.IO.File]::WriteAllText($Log, "")
$Utf8 = New-Object System.Text.UTF8Encoding($false)

function Say([string]$text, [string]$color = "Gray") {
    Write-Host $text -ForegroundColor $color
    [System.IO.File]::AppendAllText($Log, $text + "`r`n", $Utf8)
}

function Fail([string]$text) {
    Say ""
    Say "BUILD STOPPED: $text" "Red"
    Say "The full log is in out\build.log" "Yellow"
    Read-Host "Press Enter to close"
    exit 1
}

# Runs a program, shows its output, copies it to the log, and stops on failure.
function Run([string]$exe, [string[]]$arguments, [string]$what) {
    Say ("> " + $exe + " " + ($arguments -join " ")) "DarkGray"
    & $exe @arguments 2>&1 | ForEach-Object {
        $line = $_.ToString()
        Write-Host $line
        [System.IO.File]::AppendAllText($Log, $line + "`r`n", $Utf8)
    }
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)." }
}

Say "=== Gundam Operation Troy recomp build ===" "Cyan"
Say ("Started " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss") + " in " + $Root)

# --- 1. SDK helper libraries ---------------------------------------------------
if (-not (Test-Path "thirdparty\rexglue-sdk\thirdparty\fmt\CMakeLists.txt")) {
    if (-not (Test-Path ".git")) {
        Fail "This folder was not downloaded with git. Get it with: git clone https://github.com/Hatdagger/Mobile-Suit-Gundam-Operation-Troy"
    }
    Say "Downloading the SDK's helper libraries (first run only, about 400 MB)..." "Cyan"
    Run "git" @("submodule", "update", "--init", "--depth", "1", "--jobs", "8") "Downloading helper libraries"
}

# --- 2. Visual Studio C++ tools and clang --------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "Visual Studio (or its Build Tools) is not installed. Install 'Build Tools for Visual Studio 2022' with 'Desktop development with C++' and 'C++ Clang tools for Windows'."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail "Visual Studio was found, but without the C++ tools ('Desktop development with C++')." }
Say "Visual Studio: $vs"

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "call `"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}
if (-not $env:VCToolsInstallDir) { Fail "Could not load the Visual Studio C++ environment (vcvars64.bat)." }

$clangDirs = @((Join-Path $vs "VC\Tools\Llvm\x64\bin"), "C:\Program Files\LLVM\bin")
$clangDir = $null
foreach ($dir in $clangDirs) {
    $exe = Join-Path $dir "clang++.exe"
    if (Test-Path $exe) {
        $versionLine = (& $exe --version | Select-Object -First 1)
        if ($versionLine -match "version (\d+)\.") {
            if ([int]$Matches[1] -ge 18) { $clangDir = $dir; Say "Compiler: $versionLine"; break }
            Say "Skipping $exe (version $($Matches[1]); 18 or newer is needed)" "Yellow"
        }
    }
}
if (-not $clangDir) {
    Fail "clang 18 or newer was not found. In the Visual Studio Installer, add 'C++ Clang tools for Windows', or install LLVM from https://github.com/llvm/llvm-project/releases"
}
$env:Path = "$clangDir;$env:Path"

# CMake: on PATH (Visual Studio's own copy is added by vcvars64), or a standalone install.
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    foreach ($dir in @((Join-Path $env:ProgramFiles "CMake\bin"), (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"))) {
        if (Test-Path (Join-Path $dir "cmake.exe")) { $env:Path = "$dir;$env:Path"; break }
    }
}
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    Fail "cmake was not found. In the Visual Studio Installer, add 'C++ CMake tools for Windows', or install CMake from https://cmake.org/download/"
}
$cmakeVersion = ((& cmake --version | Select-Object -First 1) -replace "cmake version ", "")
Say "CMake: $cmakeVersion"
if ($cmakeVersion -match "^(\d+)\.(\d+)") {
    if ([int]$Matches[1] -lt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -lt 25)) {
        Fail "CMake $cmakeVersion is too old (3.25 or newer is needed). Install a newer one from https://cmake.org/download/"
    }
}

# Ninja: on PATH, Visual Studio's own copy, or a copy downloaded once into out\tools\ninja.
if (-not (Get-Command "ninja" -ErrorAction SilentlyContinue)) {
    $ninjaDirs = @((Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"), (Join-Path $Root "out\tools\ninja"))
    foreach ($dir in $ninjaDirs) {
        if (Test-Path (Join-Path $dir "ninja.exe")) { $env:Path = "$dir;$env:Path"; break }
    }
}
if (-not (Get-Command "ninja" -ErrorAction SilentlyContinue)) {
    Say "Ninja (the build runner) was not found; downloading it once into out\tools\ninja..." "Cyan"
    $ninjaDir = Join-Path $Root "out\tools\ninja"
    New-Item -ItemType Directory -Force -Path $ninjaDir | Out-Null
    $zip = Join-Path $ninjaDir "ninja-win.zip"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip" -OutFile $zip
        Expand-Archive -Force -Path $zip -DestinationPath $ninjaDir
    } catch {
        Fail "Could not download Ninja ($($_.Exception.Message)). In the Visual Studio Installer, add 'C++ CMake tools for Windows' (it includes ninja)."
    }
    $env:Path = "$ninjaDir;$env:Path"
}
if (-not (Get-Command "ninja" -ErrorAction SilentlyContinue)) { Fail "ninja was not found." }
Say ("Ninja: " + (& ninja --version))

# --- 3. Game files from your own copy -------------------------------------------
$haveCode = Test-Path "game\default.xex"
$haveData = Test-Path "game\Media"
if (-not ($haveCode -and $haveData)) {
    $python = $null
    if (Get-Command "py" -ErrorAction SilentlyContinue) {
        & py -3 --version *> $null
        if ($LASTEXITCODE -eq 0) { $python = @("py", "-3") }
    }
    if (-not $python -and (Get-Command "python" -ErrorAction SilentlyContinue)) {
        & python --version *> $null
        if ($LASTEXITCODE -eq 0) { $python = @("python") }
    }
    if (-not $python) { Fail "Python 3 is needed to extract the game. Install it from https://www.python.org/downloads/" }

    if (-not $GamePath) {
        Say ""
        Say "The game files are not extracted yet." "Cyan"
        Say "Enter the folder of your Gundam Operation Troy GOD copy (the one containing 424107D6), or the full path of your .iso:"
        $GamePath = (Read-Host "Game location").Trim('"', ' ')
    }
    if (-not (Test-Path -LiteralPath $GamePath)) { Fail "Not found: $GamePath" }
    Say "Extracting the game into game\ (about 1.4 GB, first run only)..." "Cyan"
    $extractArgs = @($python | Select-Object -Skip 1) + @("tools\extract_game.py", $GamePath, "--out", "game", "--all")
    Run $python[0] $extractArgs "Extracting the game"
}

# --- 4. Convert and compile ------------------------------------------------------
$preset = "win-amd64-$Config"
$buildDir = "out\build\$preset"
$start = Get-Date
Say ""
Say "Configuring ($preset)..." "Cyan"
Run "cmake" @("--preset", $preset) "Configuring the build"

# The converted code only joins the build once CMake has seen it, so convert first,
# then configure again. Codegen re-runs only when its inputs (game files, hints) change.
Say ""
Say "Converting the game's code (builds the converter first; it skips work that is already done)..." "Cyan"
Run "cmake" @("--build", $buildDir, "--target", "gundam_operation_troy_codegen") "Converting the game's code"
if (-not (Test-Path "generated\default\sources.cmake")) {
    Fail "The game's code was not converted (generated\default\sources.cmake is missing)."
}
Say ""
Say "Configuring again with the converted code..." "Cyan"
Run "cmake" @("--preset", $preset) "Configuring the build with the converted code"

Say ""
Say "Compiling. The first build takes a long time (30-90 minutes); later builds only recompile what changed." "Cyan"
Run "cmake" @("--build", $buildDir) "Compiling"
$minutes = [int]((Get-Date) - $start).TotalMinutes

$exePath = Join-Path $Root "out\build\$preset\gundam_operation_troy.exe"
Say ""
if (Test-Path $exePath) {
    Say "BUILD FINISHED in about $minutes minutes." "Green"
    Say "Game: $exePath" "Green"
} else {
    Fail "The build finished but gundam_operation_troy.exe was not found."
}
Read-Host "Press Enter to close"
