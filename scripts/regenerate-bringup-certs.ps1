<#
.SYNOPSIS
    Regenerates the local Mosquitto bring-up server certificate for a new
    IP and (re)writes firmware/gateway/certs/bringup_certs.h with the CA /
    client cert / client key DER byte arrays.

.DESCRIPTION
    Only the SERVER certificate is tied to an IP (its Subject Alternative
    Name) — the CA and client cert/key are identity material and never
    change when your Wi-Fi IP changes. This script:
      1. Writes server_ext.cnf with the new SAN.
      2. Re-signs server.crt against the existing CA + server.key (no new
         keypair — same trust chain, just a fresh SAN).
      3. Updates start-mosquitto-tls.bat's EXPECTED_IP so its own
         mismatch check stays in sync.
      4. Converts ca.crt / client.crt / client.key to DER, then emits
         firmware/gateway/certs/bringup_certs.h containing the three byte
         arrays main_test_*.c files #include instead of hand-pasting.
      5. Copies the human-readable PEM files (ca.crt, client.crt,
         client.key) into firmware/gateway/certs/ too, so everything
         cert-related the firmware project needs lives in one folder
         instead of being split across local-test/ and firmware/.

    The broker's own CA/server/client key material lives in
    local-test/mosquitto-broker/ (gitignored — never commit that folder).
    firmware/gateway/certs/ (this script's output) is also gitignored — it
    contains the client's private key in both DER and PEM form. This
    script itself has no secrets in it and is tracked normally.

.PARAMETER BrokerIp
    The broker machine's current Wi-Fi IPv4 address (e.g. from
    `(Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias 'Wi-Fi').IPAddress`).

.EXAMPLE
    .\scripts\regenerate-bringup-certs.ps1 -BrokerIp 192.168.58.145
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$BrokerIp
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BrokerDir = Join-Path $RepoRoot "local-test\mosquitto-broker"

# All paths below are fully qualified rather than relying on the current
# working directory, since some tool-invoked PowerShell sessions do not
# reliably preserve Set-Location across the lifetime of this script.
$ServerExtCnf = Join-Path $BrokerDir "server_ext.cnf"
$ServerKey = Join-Path $BrokerDir "server.key"
$ServerCsr = Join-Path $BrokerDir "server.csr"
$ServerCrt = Join-Path $BrokerDir "server.crt"
$CaCrt = Join-Path $BrokerDir "ca.crt"
$CaKey = Join-Path $BrokerDir "ca.key"
$ClientCrt = Join-Path $BrokerDir "client.crt"
$ClientKey = Join-Path $BrokerDir "client.key"
$StartBat = Join-Path $BrokerDir "start-mosquitto-tls.bat"

# ---------------------------------------------------------------------------
# Step 1 - server_ext.cnf with the new SAN
# ---------------------------------------------------------------------------
Set-Content -Path $ServerExtCnf -Value "subjectAltName = IP:$BrokerIp,DNS:localhost,IP:127.0.0.1" -NoNewline
Write-Host "[1/4] server_ext.cnf updated: SAN = IP:$BrokerIp,DNS:localhost,IP:127.0.0.1" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Step 2 - re-sign server.crt (reuse the existing server.key and CA)
# ---------------------------------------------------------------------------
& openssl req -new -key $ServerKey -out $ServerCsr -subj "/CN=iot-environmental-monitor-broker"
if ($LASTEXITCODE -ne 0) { throw "openssl req (server CSR) failed" }

& openssl x509 -req -in $ServerCsr -CA $CaCrt -CAkey $CaKey -CAcreateserial -out $ServerCrt -days 3650 -extfile $ServerExtCnf
if ($LASTEXITCODE -ne 0) { throw "openssl x509 (server cert signing) failed" }

Write-Host "[2/4] server.crt re-signed against existing CA (unchanged CA + server key)" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Step 3 - keep start-mosquitto-tls.bat's own mismatch check in sync
# ---------------------------------------------------------------------------
(Get-Content $StartBat) -replace 'set EXPECTED_IP=.*', "set EXPECTED_IP=$BrokerIp" |
    Set-Content $StartBat
Write-Host "[3/4] start-mosquitto-tls.bat EXPECTED_IP updated to $BrokerIp" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Step 4 - CA / client cert / client key -> DER -> bringup_certs.h,
#          plus human-readable PEM copies, all into firmware/gateway/certs/
# ---------------------------------------------------------------------------
$CertsDir = Join-Path $RepoRoot "firmware\gateway\certs"
New-Item -ItemType Directory -Force -Path $CertsDir | Out-Null
$CertsDir = (Resolve-Path $CertsDir).Path

$CaDer = Join-Path $CertsDir "_ca.der.tmp"
$ClientDer = Join-Path $CertsDir "_client.der.tmp"
$ClientKeyDer = Join-Path $CertsDir "_client_key.der.tmp"

function ConvertTo-CArray
{
    param([string]$DerPath, [string]$ArrayName)

    $bytes = [System.IO.File]::ReadAllBytes($DerPath)
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("static const uint8_t $ArrayName[] = {")
    for ($i = 0; $i -lt $bytes.Length; $i += 12)
    {
        $end = [Math]::Min($i + 11, $bytes.Length - 1)
        $chunk = $bytes[$i..$end]
        $hex = ($chunk | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
        $lines.Add("    $hex,")
    }
    $lines.Add("};")
    return ($lines -join "`r`n")
}

& openssl x509 -in $CaCrt -outform DER -out $CaDer
& openssl x509 -in $ClientCrt -outform DER -out $ClientDer
& openssl pkey -in $ClientKey -outform DER -out $ClientKeyDer
if ($LASTEXITCODE -ne 0) { throw "openssl DER conversion failed" }

$caArray = ConvertTo-CArray -DerPath $CaDer -ArrayName "s_bringup_ca_cert_der"
$clientCertArray = ConvertTo-CArray -DerPath $ClientDer -ArrayName "s_bringup_client_cert_der"
$clientKeyArray = ConvertTo-CArray -DerPath $ClientKeyDer -ArrayName "s_bringup_client_key_der"

$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

$header = @"
/**
 * @file bringup_certs.h
 * @brief AUTO-GENERATED by scripts/regenerate-bringup-certs.ps1 — DO NOT COMMIT, DO NOT EDIT BY HAND.
 *
 * Generated: $timestamp
 * Broker:    $BrokerIp`:8883
 *
 * DER-encoded CA cert, client cert, and client private key for the local
 * Mosquitto TLS bring-up broker (local-test/mosquitto-broker/). Included
 * by hardware bring-up main_test_*.c files instead of hand-pasted byte
 * arrays. Contains private key material — this whole folder is
 * gitignored.
 *
 * Regenerate whenever the broker machine's Wi-Fi IP changes:
 *   .\scripts\regenerate-bringup-certs.ps1 -BrokerIp <new-ip>
 */

#ifndef BRINGUP_CERTS_H
#define BRINGUP_CERTS_H

#include <stdint.h>

#define BRINGUP_CERTS_BROKER_ENDPOINT "$BrokerIp"

$caArray

$clientCertArray

$clientKeyArray

#endif /* BRINGUP_CERTS_H */
"@

$OutPath = Join-Path $CertsDir "bringup_certs.h"
Set-Content -Path $OutPath -Value $header
Write-Host "[4/4] bringup_certs.h written to $OutPath" -ForegroundColor Cyan

# Human-readable PEM copies alongside the header — everything the firmware
# project needs cert-wise, in one folder.
Copy-Item $CaCrt (Join-Path $CertsDir "ca.crt") -Force
Copy-Item $ClientCrt (Join-Path $CertsDir "client.crt") -Force
Copy-Item $ClientKey (Join-Path $CertsDir "client.key") -Force

Remove-Item $CaDer, $ClientDer, $ClientKeyDer, $ServerCsr -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done. Broker endpoint: ${BrokerIp}:8883" -ForegroundColor Green
Write-Host "All cert material for firmware is in: $CertsDir" -ForegroundColor Green
