<#
  setup.ps1 - the Quick start (README, Getting Started). Setup.cmd runs this.

  From a plain download to a launcher in this folder:
    1. check the tools (Python 3, CMake, Visual Studio 2022 C++ build tools,
       git; ffmpeg is optional) and offer to install what is missing
    2. find your copy of Epic Pinball: an installed copy (GOG or DOS), or
       GOG's offline installer, which it unpacks with innoextract (asked first)
    3. copy the game files into original\
    4. fetch the pcrecomp toolkit next to this folder
    5. configure the build, then tools\discover.py: lift, build, play every
       table headless, lift again, until nothing new turns up
    6. put a shortcut in this folder

  Every step is skipped when its result is already there, so running it again
  after a failure carries on from where it stopped. Details go to setup.log.
  It runs exactly the commands README "Step by step" lists.

  Nothing is downloaded or bundled from the game: the files come from your own
  copy, and everything generated from them stays in work\ and build\.
#>
param([string]$GameDir = "", [switch]$Yes)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root
$Log = Join-Path $Root "setup.log"
"=== setup $(Get-Date -Format s)" | Out-File $Log -Encoding utf8

# The toolkit revision this game needs (README, "Building from source"). A
# commit, not a branch: the branch moves, this build should not.
$PcrecompUrl = "https://github.com/sp00nznet/pcrecomp.git"
$PcrecompRef = "1dfdaef3a51c670e5931b0e5ef1a3b6e6cd9383e"  # pcrecomp#74, #75

function Say($m) { Write-Host $m; $m | Out-File $Log -Append -Encoding utf8 }
function Fail($m) {
    Write-Host ""
    Write-Host $m -ForegroundColor Red
    Write-Host "Details are in $Log"
    exit 1
}
function Run($what, [scriptblock]$cmd) {
    Say "  $what"
    # native tools write progress to stderr; under "Stop" PowerShell 5.1 would
    # turn every such line into a terminating error
    $ErrorActionPreference = "Continue"
    $out = & $cmd 2>&1
    $out | Out-File $Log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { Fail "$what failed." }
}
function Ask($q) {
    if ($Yes) { return $true }
    $a = Read-Host "$q [y/N]"
    return $a -match '^(y|yes)$'
}
function Have($exe) { return [bool](Get-Command $exe -ErrorAction SilentlyContinue) }
function Install($n) {
    Say "Missing: $($n.name) -- needed because it $($n.why). Download: $($n.size)."
    if (-not (Have "winget")) { Fail "Install $($n.name) yourself (winget is not available), then run Setup again." }
    if (-not (Ask "Install $($n.name) now with winget?")) { Fail "Setup needs $($n.name). Install it and run Setup again." }
    $wargs = @("install", "--id", $n.id, "-e", "--accept-package-agreements", "--accept-source-agreements")
    if ($n.args) { $wargs += $n.args }
    Run "installing $($n.name)" { winget @wargs }
    # a fresh install is on the machine PATH but not in this window's
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
}

# ---- 1. tools ---------------------------------------------------------------
Say "Checking tools..."
# `python` may be the Microsoft Store alias that only opens the Store; the
# py launcher is what a python.org install puts on PATH.
function Find-Python { if (Have "py") { "py" } elseif ((Have "python") -and ((& python --version 2>&1) -match "Python 3")) { "python" } else { $null } }
if (-not (Find-Python)) { Install @{ name = "Python 3"; id = "Python.Python.3.12"; size = "about 30 MB"; why = "runs the lifter (tools\lift.py)" } }
$py = Find-Python
if (-not (Have "cmake")) { Install @{ name = "CMake"; id = "Kitware.CMake"; size = "about 35 MB"; why = "configures the build" } }
if (-not (Have "git")) { Install @{ name = "Git"; id = "Git.Git"; size = "about 60 MB"; why = "fetches the pcrecomp toolkit" } }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs) { Install @{ name = "Visual Studio 2022 Build Tools (C++)"; id = "Microsoft.VisualStudio.2022.BuildTools"; size = "about 2 GB"; why = "compiles the lifted C"; args = "--override `"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"" } }
if (-not (Have "ffmpeg")) { Say "  (ffmpeg not found: everything works except --record. winget install Gyan.FFmpeg adds it.)" }

# ---- 2 + 3. the game ----------------------------------------------------------------
$dest = Join-Path $Root "original"
function Has-Game($dir) { return (Test-Path (Join-Path $dir "PINBALL.EXE")) -and (Test-Path (Join-Path $dir "EP1.EXE")) }

function Find-Installed {
    $c = @()
    if ($GameDir) { $c += $GameDir }
    foreach ($k in "HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1965302672", "HKLM:\SOFTWARE\GOG.com\Games\1965302672") {
        $p = (Get-ItemProperty $k -ErrorAction SilentlyContinue).path
        if ($p) { $c += $p }
    }
    $c += "C:\GOG Games\Epic Pinball", "C:\GOG Games\Epic Pinball The Complete Collection", "C:\PINBALL", "C:\EPIC\PINBALL"
    foreach ($d in $c) { if ($d -and (Has-Game $d)) { return $d } }
    return $null
}

function Find-Installer {
    $c = @()
    if ($GameDir -and (Test-Path $GameDir -PathType Leaf)) { $c += $GameDir }
    $c += Get-ChildItem (Join-Path $env:USERPROFILE "Downloads"), $Root -Filter "setup_epic_pinball*.exe" -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
    return $c | Select-Object -First 1
}

if (Has-Game $dest) {
    Say "Epic Pinball: already in original\."
} else {
    $src = Find-Installed
    if (-not $src) {
        $inst = Find-Installer
        if ($inst) {
            Say "Found GOG's installer: $inst"
            if (-not (Have "innoextract")) { Install @{ name = "innoextract"; id = "dscharrer.innoextract"; size = "about 5 MB"; why = "unpacks GOG's installer without running it" } }
            $tmp = Join-Path $Root "work\gog"
            New-Item -ItemType Directory -Force $tmp | Out-Null
            Run "unpacking the installer (nothing is run)" { innoextract --silent --output-dir $tmp $inst }
            if (Has-Game $tmp) { $src = $tmp }
        }
    }
    if (-not $src -and -not $Yes) {
        $ans = Read-Host "Where is Epic Pinball? (the folder holding PINBALL.EXE, or GOG's setup_epic_pinball*.exe)"
        if ($ans -and (Has-Game $ans)) { $src = $ans }
        elseif ($ans -and (Test-Path $ans -PathType Leaf)) { $GameDir = $ans; Fail "Run Setup again with -GameDir `"$ans`" to unpack that installer." }
    }
    if (-not $src) { Fail "Epic Pinball was not found. Run Setup again and give the folder that holds PINBALL.EXE." }
    Say "Epic Pinball: copying from $src"
    New-Item -ItemType Directory -Force $dest | Out-Null
    # the game's own files only -- not the DOSBox GOG wraps it in
    Get-ChildItem $src -File | Where-Object { $_.Extension -notin ".conf", ".pdf", ".map", ".bat", ".ico", ".info", ".hashdb", ".script", ".ini", ".dll" } |
        Copy-Item -Destination $dest
}

# ---- 4. the toolkit ----------------------------------------------------------------
if ($env:PCRECOMP_HOME -and (Test-Path "$env:PCRECOMP_HOME\tools\lift\lift16.py")) {
    $pcr = $env:PCRECOMP_HOME
} else {
    $pcr = Join-Path (Split-Path $Root -Parent) "pcrecomp"
    if (-not (Test-Path "$pcr\tools\lift\lift16.py")) {
        Run "fetching pcrecomp into $pcr" { git clone --quiet $PcrecompUrl $pcr }
    }
    Run "checking out pcrecomp $PcrecompRef" { git -C $pcr fetch --quiet origin; git -C $pcr checkout --quiet $PcrecompRef }
}
$env:PCRECOMP_HOME = $pcr

# ---- 5. lift + build -------------------------------------------------------------
Run "configuring the build" { cmake -B build -G "Visual Studio 17 2022" -A x64 "-DPCRECOMP_HOME=$pcr" }
Run "lifting and building, then playing each table headless to find the rest (a few minutes)" { & $py tools\discover.py }

# ---- 6. shortcut -----------------------------------------------------------------
$exe = Join-Path $Root "build\Release\epicpinball.exe"
if (-not (Test-Path $exe)) { Fail "The build finished but $exe is missing." }
$lnk = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $Root "Epic Pinball.lnk"))
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = $Root
$lnk.Save()
Say "Ready: Epic Pinball.lnk"
Say "Done. Double-click it to play. Shift keys flip, Space plunges, Alt+Enter toggles fullscreen."
