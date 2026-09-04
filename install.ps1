# Rigicon Live - one-shot installer for Windows.
#
# Kullanim (PowerShell'de):
#   iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/install.ps1 | iex
#
# Ne yapar:
#   1. GitHub Releases'ten en son RigiconLive.exe'yi indirir.
#   2. %LOCALAPPDATA%\Programs\RigiconLive\ altina koyar.
#   3. Kullanici PATH'ine ekler (Sistem PATH'i degil - admin gerekmez).
#
# Sonuc: Yeni PowerShell/CMD acinca "RigiconLive" komutu direkt calisir.

$ErrorActionPreference = "Stop"

$Repo    = "Quantre34/RigiconLive"
$Asset   = "RigiconLive.exe"
$InstallDir = Join-Path $env:LOCALAPPDATA "Programs\RigiconLive"
$Target  = Join-Path $InstallDir $Asset

Write-Host "==> En son surum aranıyor..." -ForegroundColor Cyan

try {
    $release = Invoke-RestMethod -UseBasicParsing `
        -Uri "https://api.github.com/repos/$Repo/releases/latest"
} catch {
    Write-Error "GitHub API'ye erisilemedi: $($_.Exception.Message)"
    exit 1
}

$assetUrl = ($release.assets | Where-Object { $_.name -eq $Asset } | Select-Object -First 1).browser_download_url

if (-not $assetUrl) {
    Write-Error "Son surumde $Asset bulunamadi. https://github.com/$Repo/releases sayfasindan manuel indirebilirsin."
    exit 1
}

Write-Host "==> Indiriliyor: $assetUrl" -ForegroundColor Cyan

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Invoke-WebRequest -UseBasicParsing -Uri $assetUrl -OutFile $Target

# MOTW (mark-of-the-web) karantinasini kaldir - guvendigin binary
try {
    Unblock-File -Path $Target -ErrorAction SilentlyContinue
} catch {}

Write-Host "==> Kuruldu: $Target" -ForegroundColor Green

# Kullanici PATH'ine ekle (Sistem PATH'ine degil - admin gerekmez)
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $userPath) { $userPath = "" }

$dirs = $userPath -split ";" | Where-Object { $_ -and ($_ -ne "") }
if ($dirs -notcontains $InstallDir) {
    $newPath = if ($userPath) { "$userPath;$InstallDir" } else { $InstallDir }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    Write-Host "==> PATH'e eklendi (kullanici olarak)" -ForegroundColor Green
    Write-Host "    Aktif olmasi icin YENI bir PowerShell veya CMD ac." -ForegroundColor Yellow
    # Bu oturum icin de aktifleştir
    $env:Path = "$env:Path;$InstallDir"
} else {
    Write-Host "==> PATH'te zaten var" -ForegroundColor Green
}

Write-Host ""
Write-Host "[+] Basarili." -ForegroundColor Green
Write-Host ""
Write-Host "Yeni bir PowerShell/CMD ac, ardindan yaz:" -ForegroundColor White
Write-Host "    RigiconLive" -ForegroundColor Cyan
