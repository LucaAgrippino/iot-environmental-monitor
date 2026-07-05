<#
.SYNOPSIS
    Runs all quality checks for a single firmware module.

.DESCRIPTION
    Executes Ceedling unit tests, cppcheck static analysis, and clang-format
    checks for the specified module. Optionally auto-fixes clang-format
    violations in-place.

.PARAMETER Module
    The module name as it appears in the source tree (e.g. config_service,
    sensor_service, time_provider).

.PARAMETER Fix
    If specified, runs clang-format -i to fix formatting violations in-place
    instead of reporting them as errors.

.EXAMPLE
    .\scripts\test-module.ps1 -Module config_service
    .\scripts\test-module.ps1 -Module config_service -Fix
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$Module,

    [switch]$Fix
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Locate repo root (script lives in <repo>/scripts/)
# ---------------------------------------------------------------------------
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$Failed = $false

function Write-Header {
    param([string]$Title)
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor DarkCyan
    Write-Host "  $Title" -ForegroundColor Cyan
    Write-Host "================================================================" -ForegroundColor DarkCyan
}

function Write-Pass {
    param([string]$Msg)
    Write-Host "[PASS] $Msg" -ForegroundColor Green
}

function Write-Fail {
    param([string]$Msg)
    Write-Host "[FAIL] $Msg" -ForegroundColor Red
    $script:Failed = $true
}

# ---------------------------------------------------------------------------
# Step 1 - Ceedling
# ---------------------------------------------------------------------------
# Some Gateway modules have a production filename that collides with an
# identically-named Field Device file (both boards keep bare, unsuffixed
# filenames like gpio_driver.c - correct for the real per-board embedded
# builds, but ambiguous for Ceedling's host test project, whose :source:
# globs span both boards' driver trees at once). Those modules run under
# their own tests/project_gateway.yml instead of the shared
# tests/project.yml - see that file's header comment for the full reason.
# Detect this by checking whether it declares a :test_<module>: block for
# this module; if so, point Ceedling at it via CEEDLING_MAIN_PROJECT_FILE.
$AltProjectFile = "project_gateway.yml"
$AltProjectPath = Join-Path tests $AltProjectFile
$UseAltProject = (Test-Path $AltProjectPath) -and
    (Select-String -Path $AltProjectPath -Pattern ":test_${Module}:" -SimpleMatch -Quiet)

$HeaderSuffix = if ($UseAltProject) { " (via $AltProjectFile)" } else { "" }
Write-Header "Ceedling - test:test_$Module$HeaderSuffix"

Push-Location tests
try {
    if ($UseAltProject) {
        $env:CEEDLING_MAIN_PROJECT_FILE = $AltProjectFile
    }
    ceedling test:test_$Module
    $CeedlingExit = $LASTEXITCODE
} finally {
    if ($UseAltProject) {
        Remove-Item Env:\CEEDLING_MAIN_PROJECT_FILE -ErrorAction SilentlyContinue
    }
    Pop-Location
}

if ($CeedlingExit -ne 0) {
    Write-Fail "Ceedling tests failed (exit $CeedlingExit)"
} else {
    Write-Pass "Ceedling"
}

# ---------------------------------------------------------------------------
# Step 2 - Locate module source directory
# ---------------------------------------------------------------------------
# Some modules' Ceedling test target uses a long, board-suffixed name
# (gpio_driver_gw, led_driver_gw) while the actual firmware directory uses
# a short, unsuffixed name (gpio, led) shared by BOTH boards. A bare suffix
# strip isn't enough to derive the directory, and an unconstrained search
# risks matching the wrong board's same-named folder when both boards have
# one (e.g. gpio).
#
# But the board suffix is only a *hint*, not a guarantee the source lives
# under that board's tree: some modules (e.g. ModbusUartDriver) have a
# single shared implementation file that physically lives under only one
# board's directory and is compiled for both via defines. So: try the
# board-hinted tree first (fixes the same-named-folder ambiguity), then
# fall back to an unconstrained search across all of firmware/ (preserves
# modules with one shared location). Within each search root, try the
# module name, then progressively shorter underscore-separated prefixes.
$OriginalModule = $Module

$BoardHint = $null
if ($Module -match '_fd$') {
    $BoardHint = 'field-device'
} elseif ($Module -match '_gw$') {
    $BoardHint = 'gateway'
}

$BareModule = $Module -replace '_(fd|gw)$', ''

$SearchRoots = [System.Collections.Generic.List[string]]::new()
if ($BoardHint) { $SearchRoots.Add((Join-Path firmware $BoardHint)) }
$SearchRoots.Add('firmware')

$Candidates = [System.Collections.Generic.List[string]]::new()
$Candidates.Add($OriginalModule)
if ($BareModule -ne $OriginalModule) { $Candidates.Add($BareModule) }
$Parts = $BareModule -split '_'
for ($i = $Parts.Count - 1; $i -ge 1; $i--) {
    $Candidates.Add(($Parts[0..($i - 1)] -join '_'))
}

$ModuleDir = $null
foreach ($Root in $SearchRoots) {
    foreach ($Candidate in $Candidates) {
        $ModuleDir = Get-ChildItem -Path $Root -Recurse -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -eq $Candidate -and
                           $_.FullName -notmatch '\\Debug\\' -and
                           $_.FullName -notmatch '\\integration-tests\\' } |
            Select-Object -First 1
        if ($null -ne $ModuleDir) { break }
    }
    if ($null -ne $ModuleDir) { break }
}

if ($null -eq $ModuleDir) {
    Write-Fail "Module directory not found under $($SearchRoots -join ' or ') for '$OriginalModule' (tried: $($Candidates -join ', '))"
    Write-Host ""
    Write-Host "RESULT: FAILED - module directory not found." -ForegroundColor Red
    exit 1
}

$ModulePath = $ModuleDir.FullName
Write-Host "Module path: $ModulePath" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Step 3 - cppcheck
# ---------------------------------------------------------------------------
Write-Header "cppcheck - $Module"

$SuppressionsFile = Join-Path $RepoRoot "cppcheck-suppressions.txt"

if (Test-Path $SuppressionsFile) {
    cppcheck --enable=style,warning,performance `
        --suppressions-list="$SuppressionsFile" `
        --suppress=missingIncludeSystem `
        --error-exitcode=1 `
        "$ModulePath"
} else {
    Write-Host "No suppressions file found - running without it." -ForegroundColor Yellow
    cppcheck --enable=style,warning,performance `
        --suppress=missingIncludeSystem `
        --error-exitcode=1 `
        "$ModulePath"
}

if ($LASTEXITCODE -ne 0) {
    Write-Fail "cppcheck found issues"
} else {
    Write-Pass "cppcheck"
}

# ---------------------------------------------------------------------------
# Step 4 - clang-format
# ---------------------------------------------------------------------------
Write-Header "clang-format - $Module"

$SourceFiles = @(Get-ChildItem -Path $ModulePath -Recurse -Include "*.c", "*.h")

if ($SourceFiles.Count -eq 0) {
    Write-Host "No .c or .h files found in $ModulePath" -ForegroundColor Yellow
} else {
    foreach ($File in $SourceFiles) {
        if ($Fix) {
            clang-format -i $File.FullName
            Write-Host "  formatted: $($File.Name)" -ForegroundColor DarkGray
        } else {
            clang-format --dry-run --Werror $File.FullName
            if ($LASTEXITCODE -ne 0) {
                Write-Fail "clang-format violation: $($File.FullName)"
            }
        }
    }

    if (-not $Failed -or $Fix) {
        if ($Fix) {
            Write-Pass "clang-format (auto-fixed)"
        } else {
            Write-Pass "clang-format"
        }
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "================================================================" -ForegroundColor DarkCyan

if ($Failed) {
    Write-Host "  RESULT: FAILED - fix the issues above before committing." -ForegroundColor Red
    exit 1
} else {
    Write-Host "  RESULT: ALL CHECKS PASSED" -ForegroundColor Green
    exit 0
}

