# Rigicon Live - kaldirma scripti (Windows).
#
# Kullanim (PowerShell):
#   iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/uninstall.ps1 | iex
#
# Ne yapar:
#   1. %LOCALAPPDATA%\Programs\RigiconLive\ klasorunu tumuyle siler.
#   2. Kullanici PATH'inden RigiconLive dizinini kaldirir.
#   3. (Opsiyonel) Rigicon Inc. sertifikasini Trusted stores'tan cikarir.
#
# Not: PATH degisikligi mevcut oturumda etkili degildir - yeni PowerShell ac.

$ErrorActionPreference = "Stop"

$InstallDir = Join-Path $env:LOCALAPPDATA "Programs\RigiconLive"
$Removed = $false

# 1) Klasoru sil
if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir
    Write-Host "[-] Silindi: $InstallDir" -ForegroundColor Yellow
    $Removed = $true
}

# 2) Kullanici PATH'inden cikar
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -and $userPath.Contains($InstallDir)) {
    $newPath = ($userPath -split ";" | Where-Object { $_ -and ($_ -ne $InstallDir) }) -join ";"
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    $env:Path = ($env:Path -split ";" | Where-Object { $_ -and ($_ -ne $InstallDir) }) -join ";"
    Write-Host "[-] PATH'ten kaldirildi" -ForegroundColor Yellow
    $Removed = $true
}

# 3) Sertifikayi kaldir (opsiyonel)
$certThumb = (Get-ChildItem Cert:\CurrentUser\Root, Cert:\CurrentUser\TrustedPublisher `
    -ErrorAction SilentlyContinue | Where-Object { $_.Subject -match "Rigicon Inc\." }).Thumbprint | Select-Object -Unique

if ($certThumb) {
    Write-Host ""
    Write-Host "Rigicon Inc. sertifikasi da kaldirilsin mi? (E/H): " -NoNewline -ForegroundColor Cyan
    $ans = Read-Host
    if ($ans -eq "E" -or $ans -eq "e") {
        foreach ($store in @("Cert:\CurrentUser\Root", "Cert:\CurrentUser\TrustedPublisher")) {
            Get-ChildItem $store -ErrorAction SilentlyContinue |
                Where-Object { $_.Subject -match "Rigicon Inc\." } |
                Remove-Item -Force
        }
        Write-Host "[-] Sertifika kaldirildi" -ForegroundColor Yellow
        $Removed = $true
    }
}

if (-not $Removed) {
    Write-Host "Rigicon Live zaten kurulu degil." -ForegroundColor Gray
} else {
    Write-Host ""
    Write-Host "[+] Kaldirildi." -ForegroundColor Green
    Write-Host "    Yeni bir PowerShell/CMD acinca PATH temizlenmis olur." -ForegroundColor Gray
}
