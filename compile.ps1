# compile.ps1
# Compile a .gg source file to a native Windows x86-64 executable.
#
# Usage:
#   .\compile.ps1 samples\hello.gg
#   .\compile.ps1 samples\hello.gg -ShowIR
#   .\compile.ps1 samples\hello.gg -Run
#   .\compile.ps1 samples\hello.gg -ShowIR -Run

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Source,

    [switch] $ShowIR,   # print the generated LLVM IR to the console
    [switch] $Run       # run the compiled executable and show its exit code
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"   # don't throw on native-command stderr

# ---- Paths ----------------------------------------------------------------

$GG    = "$PSScriptRoot\cmake-build-debug\GG.exe"
$Clang = "C:\Program Files\mysys2\clang64\bin\clang.exe"
$Build = "$PSScriptRoot\build"

# ---- Validate inputs -------------------------------------------------------

if (!(Test-Path $GG)) {
    Write-Host "ERROR: GG.exe not found at $GG" -ForegroundColor Red
    Write-Host "       Build the project first:  cmake --build cmake-build-debug --target GG"
    exit 1
}

if (!(Test-Path $Clang)) {
    Write-Host "ERROR: clang not found at $Clang" -ForegroundColor Red
    exit 1
}

$SourceResolved = Resolve-Path $Source -ErrorAction SilentlyContinue
if (!$SourceResolved) {
    Write-Host "ERROR: source file not found: $Source" -ForegroundColor Red
    exit 1
}

$stem   = [System.IO.Path]::GetFileNameWithoutExtension($SourceResolved)
$llFile = "$Build\$stem.ll"
$exeOut = "$Build\$stem.exe"

# ---- Helper: run a native command, capture stdout+stderr separately --------
# Uses System.Diagnostics.Process to avoid PowerShell 5.1's behaviour of
# wrapping every stderr line from a native exe into an ErrorRecord, which
# prints spurious "NativeCommandError" entries even for harmless warnings.
# Returns a hashtable: @{ ExitCode = int; Stdout = string[]; Stderr = string[] }

function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)

    $psi = [System.Diagnostics.ProcessStartInfo]::new($Exe)
    # Build argument string, quoting each argument that contains spaces.
    $psi.Arguments              = ($Arguments | ForEach-Object {
        if ($_ -match '\s') { "`"$_`"" } else { $_ }
    }) -join ' '
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute        = $false

    $proc = [System.Diagnostics.Process]::new()
    $proc.StartInfo = $psi
    $proc.Start() | Out-Null

    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    $splitLines = { param($s) if ($s) { $s -split "`r?`n" | Where-Object { $_ -ne '' } } else { @() } }
    return @{
        ExitCode = $proc.ExitCode
        Stdout   = (& $splitLines $stdout)
        Stderr   = (& $splitLines $stderr)
    }
}

# ---- Step 1: GG → LLVM IR -------------------------------------------------

Write-Host ""
Write-Host "==> [1/2]  GG  $SourceResolved" -ForegroundColor Cyan

$gg = Invoke-Native $GG @("$SourceResolved", "--unsafe-ptr")

foreach ($line in $gg.Stderr) {
    if (!$line) { continue }
    if ($line -match "Error")   { Write-Host "    $line" -ForegroundColor Red    }
    elseif ($line -match "Warning") { Write-Host "    $line" -ForegroundColor Yellow }
    else                        { Write-Host "    $line" }
}

if ($gg.ExitCode -ne 0) {
    Write-Host "FAILED: GG exited with code $($gg.ExitCode)" -ForegroundColor Red
    exit 1
}

if (!(Test-Path $llFile)) {
    Write-Host "FAILED: no IR file was written (semantic errors prevent codegen)" -ForegroundColor Red
    exit 1
}

Write-Host "    wrote  $llFile" -ForegroundColor DarkGray

if ($ShowIR) {
    $separator = "-" * 72
    Write-Host ""
    Write-Host "---- LLVM IR ($llFile)"
    Write-Host $separator
    Get-Content $llFile | ForEach-Object { Write-Host "  $_" }
    Write-Host $separator
}

# ---- Step 2: LLVM IR → native executable via clang ------------------------

Write-Host ""
Write-Host "==> [2/2]  clang  $llFile" -ForegroundColor Cyan

$clang = Invoke-Native $Clang @("$llFile", "-o", "$exeOut")

foreach ($line in ($clang.Stdout + $clang.Stderr)) {
    if (!$line) { continue }
    if ($line -match "error:")   { Write-Host "    $line" -ForegroundColor Red    }
    elseif ($line -match "warning:") { Write-Host "    $line" -ForegroundColor Yellow }
    else                         { Write-Host "    $line" }
}

if ($clang.ExitCode -ne 0) {
    Write-Host "FAILED: clang exited with code $($clang.ExitCode)" -ForegroundColor Red
    exit 1
}

Write-Host "    wrote  $exeOut" -ForegroundColor DarkGray
Write-Host ""
Write-Host "OK  $exeOut" -ForegroundColor Green

# ---- Step 3 (optional): run the executable ---------------------------------

if ($Run) {
    Write-Host ""
    Write-Host "==> Running $exeOut ..." -ForegroundColor Cyan
    & $exeOut
    $exitCode = $LASTEXITCODE
    $color = if ($exitCode -eq 0) { "Green" } else { "Yellow" }
    Write-Host "    exit code: $exitCode" -ForegroundColor $color
    exit $exitCode
}
