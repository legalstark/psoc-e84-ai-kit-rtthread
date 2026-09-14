[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot '.git'))) {
    throw 'Initialize Git first; this gate reviews files returned by git ls-files.'
}

$Tracked = @(& git -C $RepoRoot ls-files)
if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed.' }
$Failures = New-Object System.Collections.Generic.List[string]

foreach ($Required in @('README.md', 'LICENSE', 'NOTICE',
        'docs/THIRD_PARTY_NOTICES.md', 'scripts/prepare_private_components.ps1',
        'scripts/build_release.ps1', 'scripts/flash.ps1',
        'libraries/HAL_Drivers/SConscript', 'rt-thread/include/rtthread.h')) {
    if ($Tracked -notcontains $Required) {
        $Failures.Add("Required tracked file is missing: $Required")
    }
}

$ForbiddenPatterns = @(
    '(^|/)(build|Debug|artifacts)/',
    '^cm33/(libraries|rt-thread|tools)/',
    '^cm55/(libraries|rt-thread)/',
    '^libraries/components/emusb-host/',
    '^docs/(guide|PROJECT_HANDOVER)\.md$',
    '^overlays/',
    'proj_cm33_s_signed\.hex$',
    '(TEST_MODEL|MNIST_U55).*(tflite|bin)$',
    '\.(o|obj|elf|map|hex|dblite|pyc)$',
    '(^|/)\.sconsign',
    '(^|/)(\.eide|\.vscode)/',
    '\.code-workspace$'
)
foreach ($Path in $Tracked) {
    foreach ($Pattern in $ForbiddenPatterns) {
        if ($Path -match $Pattern) {
            $Failures.Add("Forbidden public file: $Path")
            break
        }
    }
    $FullPath = Join-Path $RepoRoot ($Path -replace '/', '\')
    if ((Test-Path -LiteralPath $FullPath -PathType Leaf) -and
        ((Get-Item -LiteralPath $FullPath).Length -ge 100MB)) {
        $Failures.Add("GitHub 100 MiB limit exceeded: $Path")
    }
}

$ExpectedAssets = @{
    'cm55/applications/ai_kit_npu/TFLM_PERSON_U55_int8x8.tflite' = '6AF50C70D0B95E4C151E0B2C50CD284BDF04246922A62BF8523166BEBFCCD2D0'
    'cm55/applications/ai_kit_npu/TFLM_PERSON_U55_samples2.bin' = '6FC6C5DEF0138C0DC3EB940F9D2A7AABA7CC899F13AE0E7A0ADA49B02FF575AC'
    'cm55/licenses/TFLM_PERSON_DETECTION_APACHE-2.0.txt' = 'CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30'
}
foreach ($Entry in $ExpectedAssets.GetEnumerator()) {
    if ($Tracked -notcontains $Entry.Key) {
        $Failures.Add("Licensed NPU asset is not tracked: $($Entry.Key)")
        continue
    }
    $AssetPath = Join-Path $RepoRoot ($Entry.Key -replace '/', '\')
    $Actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $AssetPath).Hash
    if ($Actual -ne $Entry.Value) {
        $Failures.Add("NPU asset identity mismatch: $($Entry.Key)")
    }
}

$PortableFiles = @(
    'cm33/.cproject', 'cm55/.cproject',
    'cm33/.settings/projcfg.ini', 'cm55/.settings/projcfg.ini',
    'cm33/Kconfig', 'cm55/Kconfig',
    'cm33/rtconfig.py', 'cm55/rtconfig.py',
    'cm55/firmware_bundle.json'
)
foreach ($Path in $PortableFiles) {
    $FullPath = Join-Path $RepoRoot ($Path -replace '/', '\')
    # Require a delimiter before the drive letter.  A bare [A-Za-z]: pattern
    # falsely matches the trailing "c:/" inside Eclipse ${workspace_loc:/...}.
    if ((Get-Content -LiteralPath $FullPath -Raw) -match '(^|["=;,\s>])[A-Za-z]:[\\/]') {
        $Failures.Add("Host-specific absolute path: $Path")
    }
}

if ($Failures.Count -ne 0) {
    throw ("Public release gate failed:`n - " + ($Failures -join "`n - "))
}
Write-Host "Public release gate PASS ($($Tracked.Count) tracked files)."
