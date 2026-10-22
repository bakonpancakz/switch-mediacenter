# Builds MediaCenter.nro with devkitPro's bundled msys2 and puts it in
# dist/switch/mediacenter (copy dist/switch to the root of the SD card).
# The PC tool builds with pc/build.ps1 (it goes to dist/pc).
# Usage: ./build.ps1 [all|clean]
param(
    [string]$Target = "all"
)

# toolchain.local.ps1 (next to build.ps1, not committed) can point at your
# toolchains: $env:DEVKITPRO_WIN = "D:\devkitPro"; $env:UCRT64_BIN = "D:\msys64\ucrt64\bin"
$local = Join-Path $PSScriptRoot "toolchain.local.ps1"
if (Test-Path $local) { . $local }
$dkp = $env:DEVKITPRO_WIN
if (-not $dkp) { $dkp = "C:\devkitPro" }
$bash = Join-Path $dkp "msys2\usr\bin\bash.exe"
if (-not (Test-Path $bash)) { throw "devkitPro msys2 not found at $dkp (set DEVKITPRO_WIN)" }

function ToMsys([string]$p) { "/" + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/') }
$root = ToMsys $PSScriptRoot
$env:DEVKITPRO_DIR = ToMsys $dkp

function Invoke-Make([string]$makeArgs) {
    & $bash -lc "source '$root/scripts/devkitpro-env.sh' && cd '$root' && make -j8 $makeArgs"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Target -eq "clean") {
    Invoke-Make "clean"
    exit 0
}

Invoke-Make ""

$out = Join-Path $PSScriptRoot "dist\switch\mediacenter"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $PSScriptRoot "MediaCenter.nro") (Join-Path $out "MediaCenter.nro")
Write-Host "MediaCenter.nro is in $out"
