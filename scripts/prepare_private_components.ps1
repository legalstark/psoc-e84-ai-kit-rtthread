[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SecureHex,
    [Parameter(Mandatory = $true)]
    [switch]$AcceptInfineonEula
)

$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$SecureHex = [System.IO.Path]::GetFullPath($SecureHex)
$ExpectedSecureSha256 = '5136A31B0C06F2C25004A16D2D480B5FA13C7F17509AA9BE4BF4442254C36719'
$EmusbCommit = '16931599c5305c7dd30b10986d9aa01159ec3769'
$EmusbTarget = Join-Path $RepoRoot 'libraries\components\emusb-host'
$SecureTarget = Join-Path $RepoRoot 'cm33\tools\secure\proj_cm33_s_signed.hex'

function Assert-RepoChild([string]$Path) {
    $Resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not $Resolved.StartsWith($RepoRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the repository: $Resolved"
    }
    return $Resolved
}

if (-not $AcceptInfineonEula) {
    throw 'Read the Infineon emUSB-Host license, then pass -AcceptInfineonEula.'
}
if (-not (Test-Path -LiteralPath $SecureHex -PathType Leaf)) {
    throw "Secure image is missing: $SecureHex"
}
$SecureHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $SecureHex).Hash
if ($SecureHash -ne $ExpectedSecureSha256) {
    throw "Unexpected Secure image SHA256: $SecureHash"
}

if (Test-Path -LiteralPath $EmusbTarget) {
    Remove-Item -LiteralPath (Assert-RepoChild $EmusbTarget) -Recurse -Force
}
& git clone --depth 1 --branch release-v2.3.0 `
    https://github.com/Infineon/emusb-host.git $EmusbTarget
if ($LASTEXITCODE -ne 0) { throw 'Failed to clone Infineon emUSB-Host.' }
$ActualCommit = (& git -C $EmusbTarget rev-parse HEAD).Trim()
if ($ActualCommit -ne $EmusbCommit) {
    throw "Unexpected emUSB-Host commit: $ActualCommit"
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'emusb-host.SConscript') `
    -Destination (Join-Path $EmusbTarget 'SConscript') -Force

New-Item -ItemType Directory -Path (Split-Path -Parent $SecureTarget) -Force | Out-Null
Copy-Item -LiteralPath $SecureHex -Destination $SecureTarget -Force

Write-Host 'Private components prepared.'
Write-Host "emUSB-Host: $ActualCommit"
Write-Host "Secure HEX: $SecureHash"
