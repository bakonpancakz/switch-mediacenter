# Builds MediaCenterSync.exe and its tests with msys2 ucrt64's g++ (static, so
# the exe runs on its own).
#   ./build.ps1            the exe, and a copy without debug info in dist/pc
#   ./build.ps1 -NoDist    the exe only (dist/pc's copy may be running)
#   ./build.ps1 test       every test/*_test.cpp
param([string]$Target = "all", [switch]$NoDist)
$ErrorActionPreference = "Continue"  # g++ warnings on stderr are not failures
# (toolchain.local.ps1 in the folder above can set $env:UCRT64_BIN: see ../build.ps1)
$local = Join-Path $PSScriptRoot "..\toolchain.local.ps1"
if (Test-Path $local) { . $local }
$ucrt = $env:UCRT64_BIN
if (-not $ucrt) { $ucrt = "C:\msys64\ucrt64\bin" }
$env:PATH = "$ucrt;$env:PATH"
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force obj | Out-Null
    $flags = "-std=gnu++20", "-O2", "-g", "-Wall", "-Wno-sign-compare", "-DUNICODE", "-D_UNICODE",
        "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00", "-Isrc", "-I../shared", "-I../vendor/cjson", "-I../vendor/sqlite"
    $libs = "-lole32", "-loleaut32", "-luuid", "-lgdi32", "-lcomctl32", "-lshell32", "-lshlwapi", "-lwindowscodecs", "-lwinhttp",
        "-lbcrypt", "-lmfplat", "-lmfreadwrite", "-lmfuuid", "-lshcore", "-lruntimeobject", "-lpropsys", "-lz"

    # vendored C sources, compiled once
    $vendor = @()
    foreach ($v in @(@{src = "../vendor/sqlite/sqlite3.c"; obj = "obj/sqlite3.o"; opts = "-DSQLITE_THREADSAFE=2", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_DQS=0"},
                     @{src = "../vendor/cjson/cJSON.c"; obj = "obj/cJSON.o"; opts = @()})) {
        if (-not (Test-Path $v.obj)) {
            gcc -O2 -c $v.src -o $v.obj @($v.opts)
            if ($LASTEXITCODE) { exit $LASTEXITCODE }
        }
        $vendor += $v.obj
    }

    # everything but the window, compiled once and linked into the exe and each test
    $objs = @()
    foreach ($s in @(Get-ChildItem src/*.cpp | Where-Object Name -ne "main.cpp") + @(Get-Item ../shared/vname.cpp)) {
        $o = "obj/$($s.BaseName).o"
        g++ @flags -c $s.FullName -o $o
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
        $objs += $o
    }

    if ($Target -eq "test") {
        # the tests' log goes here, not into the real %LOCALAPPDATA%\MediaCenterSync
        $env:MCSYNC_DATA_DIR = Join-Path $env:TEMP "mcsync-test-data"
        $failed = 0
        foreach ($t in Get-ChildItem test/*_test.cpp) {
            $exe = "obj/$($t.BaseName).exe"
            g++ @flags -Itest $t.FullName @objs @vendor -o $exe -static @libs
            if ($LASTEXITCODE) { $failed++; continue }
            Write-Host "== $($t.BaseName)"
            & $exe
            if ($LASTEXITCODE) { $failed++ }
        }
        exit $failed
    }

    windres app.rc -O coff -o obj/app-res.o
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    g++ @flags -municode -mwindows src/main.cpp @objs @vendor obj/app-res.o -o MediaCenterSync.exe -static @libs
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($NoDist) { "built MediaCenterSync.exe"; exit 0 }
    # a copy without debug info for dist/pc (this one keeps it, for crash logs)
    New-Item -ItemType Directory -Force ../dist/pc | Out-Null
    strip MediaCenterSync.exe -o ../dist/pc/MediaCenterSync.exe
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    "built MediaCenterSync.exe (and dist/pc)"
} finally { Pop-Location }
