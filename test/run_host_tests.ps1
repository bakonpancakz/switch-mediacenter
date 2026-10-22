# Builds and runs the Switch app's host-side tests (Windows, msys2 ucrt64 g++):
#   - epub:    the EPUB loader on a small generated book
#   - dsp:     the Spotify equalizer filters, presets and spectrum analyzer
#   - vname:   video file name parsing (shows, seasons, anime numbering, movies)
#   - library: the Switch's library.db reader on the Switch's own SQLite file layer
# The PC tool has its own tests: pc/build.ps1 test.
$ErrorActionPreference = "Stop"
# (toolchain.local.ps1 in the folder above can set $env:UCRT64_BIN: see ../build.ps1)
$local = Join-Path $PSScriptRoot "..\toolchain.local.ps1"
if (Test-Path $local) { . $local }
$ucrt = $env:UCRT64_BIN
if (-not $ucrt) { $ucrt = "C:\msys64\ucrt64\bin" }
$env:PATH = "$ucrt;$env:PATH"
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $env:TEMP "mediacenter-tests"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
g++ -std=gnu++17 -O1 -Itest/host/shim test/host/epub_test.cpp source/books/epub.cpp -o "$out/epub_test.exe"
g++ -std=gnu++17 -O1 test/host/dsp_test.cpp source/spotify/sp_dsp.cpp -o "$out/dsp_test.exe"
g++ -std=gnu++17 -O1 test/host/vname_test.cpp shared/vname.cpp -o "$out/vname_test.exe"
# the library reader with SQLite built as on the console (SQLITE_OS_OTHER: our file layer)
$sqo = "-DSQLITE_OS_OTHER=1", "-DSQLITE_THREADSAFE=0", "-DSQLITE_TEMP_STORE=3", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OMIT_WAL", "-DSQLITE_MAX_MMAP_SIZE=0", "-DSQLITE_OMIT_DEPRECATED", "-DSQLITE_DQS=0", "-DSQLITE_OMIT_SHARED_CACHE"
gcc -O1 -c @sqo vendor/sqlite/sqlite3.c -o "$out/sqlite3_other.o"
g++ -std=gnu++17 -O1 @sqo -Itest/host/lshim -Isource -Ivendor/sqlite test/host/library_test.cpp source/core/library.cpp source/core/sqlite_vfs.cpp "$out/sqlite3_other.o" -o "$out/library_test.exe"
Pop-Location

Push-Location $out
"== EPUB"
New-Item -ItemType Directory -Force "$out/epub" | Out-Null
& "$out/epub_test.exe" (($out -replace '\\', '/') + "/epub") | Select-String '^(ok|FAIL|EPUB)'
"== Equalizer / analyzer"
& "$out/dsp_test.exe" | Select-String '^(FAIL|DSP)'
"== Video file names"
& "$out/vname_test.exe" | Select-String '^(FAIL|VNAME)'
"== Library reader (Switch file layer)"
New-Item -ItemType Directory -Force "$out/library" | Out-Null
Push-Location "$out/library"
& "$out/library_test.exe" | Select-String '^(FAIL|LIBRARY|  log)'
Pop-Location
Pop-Location
