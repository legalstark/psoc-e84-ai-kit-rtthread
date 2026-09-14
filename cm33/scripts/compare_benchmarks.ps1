<#
.SYNOPSIS
Compares two validated AI Kit benchmark JSON reports.

.EXAMPLE
.\scripts\compare_benchmarks.ps1 -Candidate new.json

.EXAMPLE
.\scripts\compare_benchmarks.ps1 -Baseline old.json -Candidate new.json -GateVariable
#>
[CmdletBinding()]
param(
    [Parameter()]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$Baseline = (Join-Path (Split-Path $PSScriptRoot -Parent) 'benchmarks\baselines\pse84-ai-rtthread-v2.json'),

    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$Candidate,

    [Parameter()]
    [ValidateRange(0.0, 100.0)]
    [double]$RegressionThresholdPercent = 5.0,

    [Parameter()]
    [switch]$GateVariable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExpectedSchema = 'ai-kit-benchmark/v1'
$ExpectedSchemaVersion = 1
$HigherIsBetter = @('coremark_per_s', 'mib_per_s', 'infer_per_s')
$LowerIsBetter = @('microseconds', 'kilocycles')

function Invoke-RotateLeft5 {
    param([Parameter(Mandatory)] [uint32]$Value)
    return [uint32]((([uint64]$Value -shl 5) -bor
        ([uint64]$Value -shr 27)) -band 0xFFFFFFFFL)
}

function Get-ResultChecksum {
    param(
        [Parameter(Mandatory)] $Result,
        [Parameter(Mandatory)] [uint32]$Protocol
    )

    $executorValues = @{ m33 = 33U; m55 = 55U; u55 = 85U; none = 0U }
    $stateValues = @{ idle = 0U; running = 1U; complete = 2U; error = 3U }
    $unitValues = @{
        none = 0U
        coremark_per_s = 1U
        mib_per_s = 2U
        microseconds = 3U
        infer_per_s = 4U
        kilocycles = 5U
    }
    if (-not $executorValues.ContainsKey([string]$Result.executor) -or
        -not $stateValues.ContainsKey([string]$Result.state) -or
        -not $unitValues.ContainsKey([string]$Result.unit)) {
        throw "Unknown enum text in result '$($Result.test_id)/$($Result.executor)'."
    }

    [uint32]$checksum = 0x42454E43U
    $checksum = Invoke-RotateLeft5 ([uint32]($checksum -bxor $Protocol))
    $checksum = Invoke-RotateLeft5 ([uint32]($checksum -bxor [uint32]$Result.sequence))
    $checksum = [uint32]($checksum -bxor [uint32]$Result.test_id)
    $checksum = [uint32]($checksum -bxor ([uint32]$executorValues[[string]$Result.executor] -shl 8))
    $checksum = [uint32]($checksum -bxor ([uint32]$stateValues[[string]$Result.state] -shl 16))
    $checksum = [uint32]($checksum -bxor ([uint32]$Result.error -shl 24))
    foreach ($field in @('iterations', 'elapsed_us')) {
        $checksum = [uint32]($checksum -bxor [uint32]$Result.$field)
    }
    $checksum = [uint32]($checksum -bxor [uint32]$unitValues[[string]$Result.unit])
    foreach ($field in @('metric_x1000', 'minimum_x1000', 'maximum_x1000')) {
        $checksum = [uint32]($checksum -bxor [uint32]$Result.$field)
    }
    return $checksum
}

function Read-BenchmarkReport {
    param([Parameter(Mandatory)] [string]$Path)

    $report = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if (($report.schema -ne $ExpectedSchema) -or
        ([int]$report.schema_version -ne $ExpectedSchemaVersion) -or
        (-not [bool]$report.validated)) {
        throw "'$Path' is not a validated $ExpectedSchema report."
    }
    if (@($report.results).Count -ne [int]$report.catalog_count) {
        throw "'$Path' catalog_count does not match its result count."
    }
    return $report
}

function Get-ResultMap {
    param([Parameter(Mandatory)] $Report)

    $map = @{}
    foreach ($result in $Report.results) {
        $key = "$($result.test_id)/$($result.executor)"
        if ($map.ContainsKey($key)) {
            throw "Duplicate result key '$key'."
        }
        $computed = Get-ResultChecksum -Result $result -Protocol ([uint32]$Report.protocol)
        if ([uint32]$result.checksum -ne $computed) {
            throw "Result checksum mismatch for '$key'."
        }
        $map[$key] = $result
    }
    return $map
}

$baselineReport = Read-BenchmarkReport -Path $Baseline
$candidateReport = Read-BenchmarkReport -Path $Candidate

foreach ($field in @('status_abi', 'protocol', 'catalog_count')) {
    if ([string]$baselineReport.$field -ne [string]$candidateReport.$field) {
        throw "Reports are incompatible: '$field' differs ($($baselineReport.$field) vs $($candidateReport.$field))."
    }
}

$baselineMap = Get-ResultMap $baselineReport
$candidateMap = Get-ResultMap $candidateReport
if ($baselineMap.Count -ne $candidateMap.Count) {
    throw 'Reports have different result-key counts.'
}

$rows = [System.Collections.Generic.List[object]]::new()
$regressions = 0
foreach ($key in ($baselineMap.Keys | Sort-Object)) {
    if (-not $candidateMap.ContainsKey($key)) {
        throw "Candidate is missing result '$key'."
    }

    $old = $baselineMap[$key]
    $new = $candidateMap[$key]
    if (($old.name -ne $new.name) -or ($old.unit -ne $new.unit)) {
        throw "Result identity or unit changed for '$key'."
    }

    $isVariable = ([string]$old.name).Contains('_active_') -or
        ([string]$old.name).Contains('_peak_throughput') -or
        ([string]$old.name).Contains('_peak_hot_wall') -or
        ([string]$old.name).Contains('_model_init') -or
        ([string]$old.name).EndsWith('_first')
    $gated = $GateVariable -or (-not $isVariable)
    $status = 'SKIP'
    $change = $null
    $elapsedChange = $null

    if (($old.state -eq 'complete') -and ($new.state -ne 'complete')) {
        $status = if ($gated) { 'REGRESSION' } else { 'INFO' }
        if ($gated) { $regressions++ }
    }
    elseif (($old.state -eq 'complete') -and ($new.state -eq 'complete')) {
        $oldMetric = [double]$old.metric_x1000
        $newMetric = [double]$new.metric_x1000
        if ($oldMetric -gt 0.0) {
            if ($HigherIsBetter -contains [string]$old.unit) {
                $change = (($newMetric - $oldMetric) / $oldMetric) * 100.0
            }
            elseif ($LowerIsBetter -contains [string]$old.unit) {
                $change = (($oldMetric - $newMetric) / $oldMetric) * 100.0
            }
        }
        if ([double]$old.elapsed_us -gt 0.0) {
            $elapsedChange = (([double]$new.elapsed_us - [double]$old.elapsed_us) /
                [double]$old.elapsed_us) * 100.0
        }

        if ($null -eq $change) {
            $status = 'INFO'
        }
        elseif (-not $gated) {
            $status = 'INFO'
        }
        elseif ($change -lt -$RegressionThresholdPercent) {
            $status = 'REGRESSION'
            $regressions++
        }
        else {
            $status = 'PASS'
        }
    }

    $rows.Add([pscustomobject]@{
        Key = $key
        Name = $old.name
        Unit = $old.unit
        Baseline = if ($old.state -eq 'complete') { [double]$old.metric_x1000 / 1000.0 } else { $null }
        Candidate = if ($new.state -eq 'complete') { [double]$new.metric_x1000 / 1000.0 } else { $null }
        BetterPercent = if ($null -ne $change) { [Math]::Round($change, 3) } else { $null }
        ElapsedPercent = if ($null -ne $elapsedChange) { [Math]::Round($elapsedChange, 3) } else { $null }
        Gate = if ($gated) { 'yes' } else { 'no' }
        Status = $status
    })
}

$comparable = @($rows | Where-Object { $null -ne $_.BetterPercent }).Count
$gatedCount = @($rows | Where-Object { $_.Gate -eq 'yes' -and $null -ne $_.BetterPercent }).Count
$rows | Format-Table Key, Name, Unit, Baseline, Candidate, BetterPercent,
    ElapsedPercent, Gate, Status -AutoSize

Write-Host "Comparable=$comparable, gated=$gatedCount, regressions=$regressions, threshold=$RegressionThresholdPercent%."
Write-Host 'BetterPercent is direction-normalized: positive is better; ElapsedPercent positive means longer wall time.'
if (-not $GateVariable) {
    Write-Host 'System-active, priority-peak wall/throughput, and one-shot cold-start rows are informational. Use -GateVariable to gate them.'
}

if ($regressions -gt 0) {
    exit 2
}
