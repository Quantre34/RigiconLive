# Rigicon Live - Sign the compiled .exe with the Rigicon Inc. certificate
#
# Kullanim:
#   .\sign-exe.ps1                                  # varsayilan .pfx + .exe
#   .\sign-exe.ps1 -Exe path\to\RigiconLive.exe -Pfx path\to\RigiconInc.pfx

[CmdletBinding()]
param(
    [string]$Exe = "",
    [string]$Pfx = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot   = Resolve-Path (Join-Path $scriptRoot "..\..\..")

if (-not $Exe) { $Exe = Join-Path $repoRoot "dist\windows\RigiconLive.exe" }
if (-not $Pfx) { $Pfx = Join-Path $repoRoot "build\certs\RigiconInc.pfx"  }

if (-not (Test-Path $Exe)) { throw "Executable bulunamadi: $Exe. Once build.bat calistir." }
if (-not (Test-Path $Pfx)) { throw ".pfx bulunamadi: $Pfx. Once generate-cert.ps1 calistir." }

# Locate signtool.exe (from Windows 10/11 SDK)
$signtool = $null
$roots = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
    "${env:ProgramFiles}\Windows Kits\10\bin"
) | Where-Object { $_ -and (Test-Path $_) }

foreach ($root in $roots) {
    $found = Get-ChildItem -Path $root -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match "\\x64\\" } |
             Sort-Object FullName -Descending
    if ($found -and $found.Count -gt 0) { $signtool = $found[0].FullName; break }
}

if (-not $signtool) {
    throw "signtool.exe bulunamadi. Windows 10/11 SDK kurulu olmali."
}

Write-Host "[*] signtool  : $signtool"
Write-Host "[*] executable: $Exe"
Write-Host "[*] cert      : $Pfx"

$pwSecure = Read-Host -AsSecureString -Prompt ".pfx parolasi"
$bstr     = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($pwSecure)
$pwPlain  = [System.Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
[System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) | Out-Null

& $signtool sign `
    /f  $Pfx `
    /p  $pwPlain `
    /tr $TimestampUrl `
    /td sha256 `
    /fd sha256 `
    /d  "Rigicon Live" `
    /du "https://rigicon.com" `
    $Exe

if ($LASTEXITCODE -ne 0) { throw "signtool sign basarisiz (kod $LASTEXITCODE)" }

Write-Host ""
Write-Host "[*] Imza dogrulaniyor..."
& $signtool verify /pa /v $Exe
if ($LASTEXITCODE -ne 0) { throw "verify basarisiz (kod $LASTEXITCODE)" }

Write-Host ""
Write-Host "[+] Imzalandi: $Exe" -ForegroundColor Green
Write-Host "    Ozellikler > Dijital Imzalar > 'Rigicon Inc.' gorunecek."
