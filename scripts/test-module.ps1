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
Write-Header "Ceedling - test:test_$Module"

Push-Location tests
ceedling test:test_$Module
$CeedlingExit = $LASTEXITCODE
Pop-Location

if ($CeedlingExit -ne 0) {
    Write-Fail "Ceedling tests failed (exit $CeedlingExit)"
} else {
    Write-Pass "Ceedling"
}

# ---------------------------------------------------------------------------
# Step 2 - Locate module source directory
# ---------------------------------------------------------------------------
if ( ($Module.EndsWith( "_fd" ) ) -or ($Module.EndsWith( "_gw" ) )  ) {

    $Module = $Module -replace '_fd$|_gw$', ''
}

$ModuleDir = Get-ChildItem -Path firmware -Recurse -Directory |
Where-Object { $_.Name -eq $Module -and
               $_.FullName -notmatch '\\Debug\\' -and
               $_.FullName -notmatch '\\integration-tests\\' } |
Select-Object -First 1

if ($null -eq $ModuleDir) {
    Write-Fail "Module directory '$Module' not found under firmware/"
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

