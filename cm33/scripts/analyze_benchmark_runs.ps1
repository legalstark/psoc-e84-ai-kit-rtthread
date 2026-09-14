<#
.SYNOPSIS
Summarizes repeatability across compatible AI Kit benchmark JSON reports.

.EXAMPLE
.\scripts\analyze_benchmark_runs.ps1 -Directory .\artifacts\repeatability

.EXAMPLE
.\scripts\analyze_benchmark_runs.ps1 -Directory .\artifacts\repeatability `
    -OutputPath .\artifacts\repeatability\summary.json
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Directory,

    [Parameter()]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$reportFiles = @(Get-ChildItem -LiteralPath $Directory -Filter 'benchmark-*.json' -File -Recurse |
    Sort-Object FullName)
if ($reportFiles.Count -lt 2) {
    throw 'At least two benchmark JSON reports are required.'
}

$reports = @($reportFiles | ForEach-Object {
    $report = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
    [pscustomobject]@{ Path = $_.FullName; Data = $report }
})

$reference = $reports[0].Data
foreach ($item in $reports) {
    $report = $item.Data
    foreach ($field in @('schema', 'schema_version', 'status_abi', 'protocol',
                          'catalog_count')) {
        if ([string]$report.$field -ne [string]$reference.$field) {
            throw "Incompatible report '$($item.Path)': field '$field' differs."
        }
    }
    if (-not $report.validated) {
        throw "Report '$($item.Path)' is not validated."
    }
}

function Get-CompleteResultMap {
    param([Parameter(Mandatory)] $Report)

    $map = @{}
    foreach ($result in @($Report.results | Where-Object { $_.state -eq 'complete' })) {
        $key = "$($result.test_id)/$($result.executor)"
        if ($map.ContainsKey($key)) {
            throw "Duplicate COMPLETE result '$key'."
        }
        $map[$key] = $result
    }
    return $map
}

$maps = @($reports | ForEach-Object { Get-CompleteResultMap -Report $_.Data })
$referenceKeys = @($maps[0].Keys | Sort-Object)
if ($referenceKeys.Count -eq 0) {
    throw 'The reports contain no COMPLETE benchmark results.'
}

foreach ($map in $maps) {
    $keys = @($map.Keys | Sort-Object)
    if (($keys -join ',') -ne ($referenceKeys -join ',')) {
        throw 'The COMPLETE benchmark key set differs between reports.'
    }
}

$summaryResults = foreach ($key in $referenceKeys) {
    $identity = $maps[0][$key]
    $values = @($maps | ForEach-Object {
        $result = $_[$key]
        if ($result.name -ne $identity.name -or $result.unit -ne $identity.unit) {
            throw "Benchmark identity differs for '$key'."
        }
        [double]$result.metric_x1000
    })

    $mean = ($values | Measure-Object -Average).Average
    $sumSquared = 0.0
    foreach ($value in $values) {
        $difference = $value - $mean
        $sumSquared += $difference * $difference
    }
    $standardDeviation = [Math]::Sqrt($sumSquared / $values.Count)
    $coefficientOfVariation = if ($mean -eq 0.0) {
        0.0
    } else {
        100.0 * $standardDeviation / [Math]::Abs($mean)
    }

    [pscustomobject][ordered]@{
        test_id = [int]$identity.test_id
        name = [string]$identity.name
        executor = [string]$identity.executor
        unit = [string]$identity.unit
        samples = $values.Count
        mean_x1000 = [Math]::Round($mean, 3)
        minimum_x1000 = [long](($values | Measure-Object -Minimum).Minimum)
        maximum_x1000 = [long](($values | Measure-Object -Maximum).Maximum)
        standard_deviation_x1000 = [Math]::Round($standardDeviation, 3)
        coefficient_of_variation_percent = [Math]::Round($coefficientOfVariation, 4)
    }
}

$summary = [ordered]@{
    schema = 'ai-kit-benchmark-repeatability/v1'
    samples = $reports.Count
    benchmark_schema = [string]$reference.schema
    status_abi = [int]$reference.status_abi
    protocol = [int]$reference.protocol
    catalog_count = [int]$reference.catalog_count
    source_reports = @($reportFiles | ForEach-Object { $_.FullName })
    results = @($summaryResults)
}

$json = $summary | ConvertTo-Json -Depth 6
if ($OutputPath) {
    $parent = Split-Path -Parent $OutputPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding utf8
    Write-Host "Summary: $OutputPath"
}

$summaryResults |
    Sort-Object test_id, executor |
    Format-Table test_id, executor, name, samples, mean_x1000,
        minimum_x1000, maximum_x1000, standard_deviation_x1000,
        coefficient_of_variation_percent -AutoSize
