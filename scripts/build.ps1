[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [int]$J = 0,
    [string]$C = "",
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
    Write-Host "Usage: .\scripts\build.ps1 [-BuildDir <dir>] [-J <jobs>] [-C <Config>] [<cmake args>...]"
    Write-Host "  -BuildDir   Build directory (default: .\build or `$env:BUILD_DIR)"
    Write-Host "  -J          Parallel jobs (default: processor count)"
    Write-Host "  -C          CMake build type (Release, Debug, etc.)"
    Write-Host "  -h          Show this help"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\scripts\build.ps1 -BuildDir .\out -J 4 -C Release"
    Write-Host "  .\scripts\build.ps1 -C Debug -DCMAKE_VERBOSE_MAKEFILE=ON"
}

if ($ExtraArgs -contains '-h' -or $ExtraArgs -contains '--help') { 
    Show-Help
    exit 0 
}

if ($J -eq 0) { $J = [Environment]::ProcessorCount }

Write-Host "[build.ps1] root: $PWD"
Write-Host "[build.ps1] build dir: $BuildDir"

$cmakeArgs = @("-S", "$PWD", "-B", $BuildDir)
if ($C) { $cmakeArgs += "-DCMAKE_BUILD_TYPE=$C" }
if ($ExtraArgs) { $cmakeArgs += $ExtraArgs }

Write-Host "[build.ps1] cmake args: $($cmakeArgs -join ' ')"

try {
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
    
    Write-Host "[build.ps1] building (-j $J)"
    & cmake --build $BuildDir -- -j $J
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }
    
    Write-Host "[build.ps1] finished. Artifacts in: $BuildDir"
} catch {
    Write-Error "Build failed: $_"
    exit 1
}
