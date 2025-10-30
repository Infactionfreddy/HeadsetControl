[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [switch]$Distclean,
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Stop'

# set BuildDir from env or default
if (-not $BuildDir) { 
    if ($env:BUILD_DIR) { 
        $BuildDir = $env:BUILD_DIR 
    } else { 
        $BuildDir = Join-Path $PWD "build" 
    } 
}

function Show-Help {
    Write-Host "Usage: .\scripts\clean.ps1 [-BuildDir <dir>] [-Distclean]"
    Write-Host "  -BuildDir   Build directory (default: .\build or `$env:BUILD_DIR)"
    Write-Host "  -Distclean  Remove entire build directory instead of running clean target"
    Write-Host "  -h          Show this help"
}

if ($ExtraArgs -contains '-h' -or $ExtraArgs -contains '--help') { 
    Show-Help
    exit 0 
}

if ($Distclean) {
    Write-Host "[clean.ps1] removing build dir: $BuildDir"
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force -LiteralPath $BuildDir -ErrorAction SilentlyContinue
    }
    Write-Host "[clean.ps1] done"
    exit 0
}

if (-not (Test-Path $BuildDir)) {
    Write-Host "[clean.ps1] build dir does not exist: $BuildDir"
    exit 0
}

Write-Host "[clean.ps1] running 'cmake --build $BuildDir --target clean'"
try {
    & cmake --build $BuildDir --target clean
    if ($LASTEXITCODE -ne 0) { 
        Write-Warning "cmake clean failed with exit code $LASTEXITCODE, removing build dir"
        Remove-Item -Recurse -Force -LiteralPath $BuildDir -ErrorAction SilentlyContinue
    }
} catch {
    Write-Warning "cmake clean threw error: $_"
    Write-Host "[clean.ps1] removing build dir as fallback"
    Remove-Item -Recurse -Force -LiteralPath $BuildDir -ErrorAction SilentlyContinue
}

Write-Host "[clean.ps1] done"
