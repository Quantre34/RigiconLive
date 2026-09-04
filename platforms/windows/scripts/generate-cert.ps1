# Rigicon Live - Self-signed code-signing certificate generator
# "Rigicon Inc." adına geliştirme/test amaçlı .cer + .pfx üretir.
# Bir defa çalıştır. Ardından sign-exe.ps1 ile executable'ı imzala.

[CmdletBinding()]
param(
    [string]$Subject      = "CN=Rigicon Inc., O=Rigicon Inc., C=TR",
    [string]$FriendlyName = "Rigicon Inc. Code Signing",
    [int]   $Years        = 5
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot   = Resolve-Path (Join-Path $scriptRoot "..\..\..")
$outDir     = Join-Path $repoRoot "build\certs"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host "[*] Sertifika olusturuluyor: $Subject" -ForegroundColor Cyan

$cert = New-SelfSignedCertificate `
    -Subject          $Subject `
    -FriendlyName     $FriendlyName `
    -Type             CodeSigningCert `
    -KeyUsage         DigitalSignature `
    -KeyLength        2048 `
    -KeyAlgorithm     RSA `
    -HashAlgorithm    SHA256 `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -NotAfter         (Get-Date).AddYears($Years)

Write-Host "    Thumbprint : $($cert.Thumbprint)" -ForegroundColor Green
Write-Host "    Subject    : $($cert.Subject)"    -ForegroundColor Green
Write-Host "    Valid until: $($cert.NotAfter)"   -ForegroundColor Green

# .cer - public certificate (safe to share, used to trust)
$cerPath = Join-Path $outDir "RigiconInc.cer"
Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null
Write-Host "[+] Public .cer  : $cerPath" -ForegroundColor Green

# .pfx - includes private key, password-protected
$pfxPath = Join-Path $outDir "RigiconInc.pfx"
$pw      = Read-Host -AsSecureString -Prompt ".pfx icin parola belirle"
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $pw | Out-Null
Write-Host "[+] Signing .pfx : $pfxPath" -ForegroundColor Green

Write-Host ""
Write-Host "GELISTIRME/TEST MAKINESINDE SERTIFIKAYI GUVEN LISTESINE EKLEMEK ICIN:" -ForegroundColor Yellow
Write-Host "  (Yonetici PowerShell'de calistir)"                                    -ForegroundColor Yellow
Write-Host "  Import-Certificate -FilePath '$cerPath' -CertStoreLocation Cert:\LocalMachine\Root"
Write-Host "  Import-Certificate -FilePath '$cerPath' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher"
Write-Host ""
Write-Host "Sonraki adim: platforms\windows\scripts\sign-exe.ps1"                    -ForegroundColor Cyan
