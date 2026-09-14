[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$StudioRoot,

    [int]$Jobs = 12
)

$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$StudioRoot = [System.IO.Path]::GetFullPath($StudioRoot)
$M33Root = Join-Path $RepoRoot 'cm33'
$M55Root = Join-Path $RepoRoot 'cm55'
$Scons = Join-Path $StudioRoot 'platform\env_released\env-new\.venv\Scripts\scons.exe'
$Toolchain = Join-Path $StudioRoot 'repo\Extract\ToolChain_Support_Packages\ARM\GNU_Tools_for_ARM_Embedded_Processors\13.3\bin'
$EdgeProtect = Join-Path $StudioRoot 'repo\Extract\Board_Support_Packages\Infineon\PSOC_E84-EDGI-TALK\1.4.0\tools\edgeprotecttools\bin\edgeprotecttools.exe'
$SecureHex = Join-Path $M33Root 'tools\secure\proj_cm33_s_signed.hex'
$env:RTT_EXEC_PATH = $Toolchain

foreach ($Required in @($Scons, $EdgeProtect, $SecureHex,
        (Join-Path $Toolchain 'arm-none-eabi-gcc.exe'))) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Missing build dependency: $Required"
    }
}

Set-Location -LiteralPath $M33Root
& $Scons "-j$Jobs" BUILD=release
if ($LASTEXITCODE -ne 0) { throw 'M33 Release build failed.' }

Set-Location -LiteralPath $M55Root
& $Scons "-j$Jobs" BUILD=release
if ($LASTEXITCODE -ne 0) { throw 'M55 Release build failed.' }

& $EdgeProtect run-config -i firmware_bundle.json
if ($LASTEXITCODE -ne 0) { throw 'Three-image bundle failed.' }

$Output = Join-Path $M55Root 'build\ai_kit_rtthread_cm33_cm55.hex'
if (-not (Test-Path -LiteralPath $Output -PathType Leaf)) {
    throw "Combined image is missing: $Output"
}
Get-Item -LiteralPath $Output | Select-Object FullName, Length, LastWriteTime
Get-FileHash -Algorithm SHA256 -LiteralPath $Output
