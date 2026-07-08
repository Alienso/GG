# debug.ps1
# Compile a .gg file WITH debug info and launch GDB on the resulting executable.
# One-command debugging from a terminal (or CLion's built-in terminal).
#
# Usage:
#   .\debug.ps1 e2e\class_test.gg              # build + open gdb
#   .\debug.ps1 e2e\class_test.gg -Break 180   # build + break at class_test.gg:180, then run
#   .\debug.ps1 e2e\class_test.gg -NoBuild     # skip the rebuild, just open gdb
#
# Requires a GDB that reads DWARF + MinGW executables. Defaults to the msys2 mingw64 gdb;
# override with -Gdb (e.g. CLion's bundled gdb under bin\gdb\win\x64\bin\gdb.exe).

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Source,

    [int]    $Break,     # optional source line to break on (then auto-run)
    [switch] $NoBuild,   # skip recompilation
    [string] $Gdb = "C:\Program Files\mysys2\mingw64\bin\gdb.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$stem   = [System.IO.Path]::GetFileNameWithoutExtension($Source)
$exeOut = "$PSScriptRoot\build\$stem.exe"

# ---- Step 1: compile with debug info (reuse compile.ps1 -Debug) ----
if (-not $NoBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\compile.ps1" $Source -DebugInfo
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: debug build did not complete." -ForegroundColor Red
        exit 1
    }
}

if (!(Test-Path $exeOut)) {
    Write-Host "ERROR: executable not found: $exeOut" -ForegroundColor Red
    Write-Host "       (run without -NoBuild to compile it first)"
    exit 1
}
if (!(Test-Path $Gdb)) {
    Write-Host "ERROR: gdb not found at $Gdb" -ForegroundColor Red
    Write-Host "       Pass -Gdb <path> (e.g. CLion's bundled gdb)."
    exit 1
}

# ---- Step 2: launch gdb ----
Write-Host ""
Write-Host "==> gdb  $exeOut" -ForegroundColor Cyan

$gdbArgs = @()
$srcName = [System.IO.Path]::GetFileName($Source)
if ($PSBoundParameters.ContainsKey('Break')) {
    $gdbArgs += @("-ex", "break ${srcName}:${Break}", "-ex", "run")
}
$gdbArgs += $exeOut

& $Gdb @gdbArgs
