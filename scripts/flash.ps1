[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$StudioRoot
)

$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$StudioRoot = [System.IO.Path]::GetFullPath($StudioRoot)
$OpenOcdBin = Join-Path $StudioRoot 'repo\Extract\Debugger_Support_Packages\Infineon\OpenOCD-Infineon\2.0.0\bin'
$OpenOcd = Join-Path $OpenOcdBin 'openocd.exe'
$Image = Join-Path $RepoRoot 'cm55\build\ai_kit_rtthread_cm33_cm55.hex'

foreach ($Required in @($OpenOcd, $Image)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Missing flash dependency: $Required"
    }
}

$ImageForOpenOcd = $Image.Replace('\', '/')
$ProgramCommand = "program {$ImageForOpenOcd} verify reset exit"

Push-Location -LiteralPath $OpenOcdBin
try {
    & $OpenOcd `
        -s ../scripts `
        -s ../flm/cypress/cat1d `
        -f interface/kitprog3.cfg `
        -f target/infineon/pse84xgxs2.cfg `
        -c $ProgramCommand
    if ($LASTEXITCODE -ne 0) {
        throw "OpenOCD flashing failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

Write-Host "Flashed and verified: $Image"
