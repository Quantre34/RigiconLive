# Rigicon Live - Rigicon Inc. sertifikasını yükle (Windows).
#
# Bu script "Rigicon Inc." self-signed code-signing sertifikasını Windows'a
# TRUSTED PUBLISHER olarak yükler. Bunu yaptıktan sonra imzalı RigiconLive.exe'yi
# indirdiğinde SmartScreen "unrecognized publisher" uyarısı vermez.
#
# Kullanim (PowerShell):
#   iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/certs/install-cert.ps1 | iex
#
# Ne yapar:
#   1. RigiconInc.cer dosyasını repodan indirir.
#   2. CurrentUser\Root (Trusted Root Certification Authorities) altına yükler
#   3. CurrentUser\TrustedPublisher altına yükler
#
# Admin gerektirmez - sadece bu kullanıcı için geçerlidir.

$ErrorActionPreference = "Stop"

$CertUrl = "https://github.com/Quantre34/RigiconLive/raw/main/certs/RigiconInc.cer"
$TmpCert = Join-Path $env:TEMP "RigiconInc.cer"

Write-Host "==> Sertifika indiriliyor..." -ForegroundColor Cyan
Invoke-WebRequest -UseBasicParsing -Uri $CertUrl -OutFile $TmpCert

Write-Host "==> Sertifika bilgileri:" -ForegroundColor Cyan
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $TmpCert
Write-Host "    Konu       : $($cert.Subject)"
Write-Host "    Veren      : $($cert.Issuer)"
Write-Host "    Parmakizi  : $($cert.Thumbprint)"
Write-Host "    Gecerlilik : $($cert.NotBefore) - $($cert.NotAfter)"

Write-Host ""
Write-Host "Bu sertifika 'Rigicon Inc.' adina, self-signed'dir." -ForegroundColor Yellow
Write-Host "Onayliyor musun? (E/H): " -NoNewline -ForegroundColor Yellow
$answer = Read-Host
if ($answer -ne "E" -and $answer -ne "e") {
    Remove-Item $TmpCert -Force
    Write-Host "Iptal edildi." -ForegroundColor Red
    exit 1
}

# Trusted Root'a ekle (CurrentUser)
$rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store `
    "Root", "CurrentUser"
$rootStore.Open("ReadWrite")
$rootStore.Add($cert)
$rootStore.Close()
Write-Host "==> Trusted Root'a eklendi (CurrentUser)" -ForegroundColor Green

# Trusted Publisher'a ekle (CurrentUser)
$pubStore = New-Object System.Security.Cryptography.X509Certificates.X509Store `
    "TrustedPublisher", "CurrentUser"
$pubStore.Open("ReadWrite")
$pubStore.Add($cert)
$pubStore.Close()
Write-Host "==> Trusted Publisher'a eklendi (CurrentUser)" -ForegroundColor Green

Remove-Item $TmpCert -Force

Write-Host ""
Write-Host "[+] Sertifika kuruldu." -ForegroundColor Green
Write-Host ""
Write-Host "Simdi imzali .exe'yi indirebilirsin (uyari cikmayacak):" -ForegroundColor White
Write-Host "    iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/install.ps1 | iex" -ForegroundColor Cyan
Write-Host ""
Write-Host "Kaldirmak icin:" -ForegroundColor Gray
Write-Host "    Get-ChildItem Cert:\CurrentUser\Root\$($cert.Thumbprint) | Remove-Item" -ForegroundColor Gray
Write-Host "    Get-ChildItem Cert:\CurrentUser\TrustedPublisher\$($cert.Thumbprint) | Remove-Item" -ForegroundColor Gray
