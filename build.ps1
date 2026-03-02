# memo build script
# Usage: ./build.ps1

$ErrorActionPreference = "Stop"

Write-Host "memo build" -ForegroundColor Cyan
Write-Host "==========" -ForegroundColor Cyan

# config.h from config.def.h
if (-not (Test-Path config.h)) {
    Copy-Item config.def.h config.h
    Write-Host "created config.h from config.def.h" -ForegroundColor Yellow
}

# compile
Write-Host "`nCompiling memo.exe..." -ForegroundColor Yellow
gcc -std=c99 -O2 -Wall -Wextra -o memo.exe memo.c gap.c `
    -lgdi32 -luser32 -lshell32 -lshcore

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed." -ForegroundColor Red
    exit 1
}

$size = (Get-Item memo.exe).Length / 1KB
Write-Host "OK - memo.exe ($([math]::Round($size, 1)) KB)" -ForegroundColor Green

Write-Host "`nDone. Files:" -ForegroundColor Cyan
Write-Host "  memo.exe          - the utility"
Write-Host "  config.h          - your config (edit and rebuild)"
Write-Host "  config.def.h      - upstream defaults"
