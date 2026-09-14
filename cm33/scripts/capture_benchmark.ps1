<#
.SYNOPSIS
Captures and validates the AI Kit RT-Thread benchmark catalog over msh.

.EXAMPLE
.\scripts\capture_benchmark.ps1 -Port COM9

.EXAMPLE
.\scripts\capture_benchmark.ps1 -Port COM9 -RunNpu

.EXAMPLE
.\scripts\capture_benchmark.ps1 -Port COM9 -RunAll
#>
[CmdletBinding()]
param(
    [Parameter()]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port = 'COM9',

    [Parameter()]
    [ValidateRange(1200, 4000000)]
    [int]$BaudRate = 115200,

    [Parameter()]
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'artifacts\benchmarks'),

    [Parameter()]
    [switch]$RunNpu,

    [Parameter()]
    [switch]$RunAll,

    [Parameter()]
    [ValidateRange(2, 120)]
    [int]$TimeoutSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ExpectedSchema = 'ai-kit-benchmark/v1'
$script:ExpectedSchemaVersion = 1
$script:ExpectedStatusAbi = 13
$script:ExpectedProtocol = 7
$script:ExpectedCatalogCount = 39
$script:Serial = $null

function Invoke-MshCommand {
    param(
        [Parameter(Mandatory)] [string]$Command,
        [Parameter()] [int]$Timeout = $TimeoutSeconds
    )

    $script:Serial.DiscardInBuffer()
    $script:Serial.Write($Command + [char]13)
    $deadline = [DateTime]::UtcNow.AddSeconds($Timeout)
    $buffer = ''

    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 40
        $buffer += $script:Serial.ReadExisting()
        if ($buffer.Contains('msh >')) {
            return $buffer
        }
    }

    $tailStart = [Math]::Max(0, $buffer.Length - 500)
    throw "Timed out waiting for msh after '$Command'. Tail: $($buffer.Substring($tailStart))"
}

function Get-MarkedPayload {
    param(
        [Parameter(Mandatory)] [string]$Raw,
        [Parameter(Mandatory)] [string]$BeginMarker,
        [Parameter(Mandatory)] [string]$EndMarker
    )

    $pattern = '(?s)' + [regex]::Escape($BeginMarker) + '\r?\n(.*?)\r?\n' +
        [regex]::Escape($EndMarker)
    $match = [regex]::Match($Raw, $pattern)
    if (-not $match.Success) {
        throw "Report markers '$BeginMarker'/'$EndMarker' were not found."
    }
    return $match.Groups[1].Value
}

function Get-JsonSnapshot {
    $raw = Invoke-MshCommand -Command 'bench_report json'
    $payload = Get-MarkedPayload -Raw $raw `
        -BeginMarker 'BEGIN_AI_KIT_BENCHMARK_JSON' `
        -EndMarker 'END_AI_KIT_BENCHMARK_JSON'
    return [pscustomobject]@{
        Payload = $payload
        Data = ($payload | ConvertFrom-Json)
    }
}

function Get-CsvSnapshot {
    $raw = Invoke-MshCommand -Command 'bench_report csv'
    $payload = Get-MarkedPayload -Raw $raw `
        -BeginMarker 'BEGIN_AI_KIT_BENCHMARK_CSV' `
        -EndMarker 'END_AI_KIT_BENCHMARK_CSV'
    return [pscustomobject]@{
        Payload = $payload
        Data = @($payload | ConvertFrom-Csv)
    }
}

function Get-ResultKey {
    param([Parameter(Mandatory)] $Result)
    return "$($Result.test_id)/$($Result.executor)"
}

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

function Get-ResultByKey {
    param(
        [Parameter(Mandatory)] $Report,
        [Parameter(Mandatory)] [string]$Key
    )
    return @($Report.results | Where-Object { (Get-ResultKey $_) -eq $Key }) | Select-Object -First 1
}

function Wait-BenchmarkCompletion {
    param(
        [Parameter(Mandatory)] [string]$Command,
        [Parameter(Mandatory)] [string]$ResultKey,
        [Parameter()] [int]$CompletionTimeout = $TimeoutSeconds
    )

    $before = Get-JsonSnapshot
    $previous = Get-ResultByKey -Report $before.Data -Key $ResultKey
    if ($null -eq $previous) {
        throw "Result key '$ResultKey' is absent before '$Command'."
    }

    $request = Invoke-MshCommand -Command $Command
    if ($request -notmatch 'started \(0\)') {
        throw "Benchmark request '$Command' was rejected: $request"
    }

    $previousSequence = [uint32]$previous.sequence
    $deadline = [DateTime]::UtcNow.AddSeconds($CompletionTimeout)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        try {
            $snapshot = Get-JsonSnapshot
        }
        catch {
            continue
        }

        $current = Get-ResultByKey -Report $snapshot.Data -Key $ResultKey
        if (($null -ne $current) -and ([uint32]$current.sequence -gt $previousSequence)) {
            if ($current.state -eq 'complete') {
                Write-Host "Completed $Command -> $ResultKey sequence=$($current.sequence)"
                return
            }
            if ($current.state -eq 'failed') {
                throw "Benchmark '$Command' failed with error $($current.error)."
            }
        }
    }

    throw "Benchmark '$Command' did not complete within $CompletionTimeout seconds."
}

function Wait-SynchronousBenchmarkCompletion {
    param(
        [Parameter(Mandatory)] [string]$Command,
        [Parameter(Mandatory)] [string]$ResultKey,
        [Parameter()] [int]$CompletionTimeout = $TimeoutSeconds
    )

    $before = Get-JsonSnapshot
    $previous = Get-ResultByKey -Report $before.Data -Key $ResultKey
    if ($null -eq $previous) {
        throw "Result key '$ResultKey' is absent before '$Command'."
    }

    $output = Invoke-MshCommand -Command $Command -Timeout $CompletionTimeout
    if ($output -match '(?i)failed') {
        throw "Synchronous benchmark '$Command' reported an error: $output"
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        $after = Get-JsonSnapshot
        $current = Get-ResultByKey -Report $after.Data -Key $ResultKey
        if (($null -ne $current) -and
            ([uint32]$current.sequence -gt [uint32]$previous.sequence)) {
            if ($current.state -eq 'complete') {
                Write-Host "Completed $Command -> $ResultKey sequence=$($current.sequence)"
                return
            }
            if ($current.state -eq 'error') {
                throw "Synchronous benchmark '$Command' failed with error $($current.error)."
            }
        }
        Start-Sleep -Milliseconds 200
    }
    throw "Synchronous benchmark '$Command' did not publish a new COMPLETE '$ResultKey' result."
}

function Assert-Reports {
    param(
        [Parameter(Mandatory)] $Json,
        [Parameter(Mandatory)] $Csv
    )

    if (($Json.schema -ne $script:ExpectedSchema) -or
        ([int]$Json.schema_version -ne $script:ExpectedSchemaVersion) -or
        ([int]$Json.status_abi -ne $script:ExpectedStatusAbi) -or
        ([int]$Json.protocol -ne $script:ExpectedProtocol) -or
        (-not [bool]$Json.validated)) {
        throw 'JSON schema, ABI, protocol, or validated flag does not match the current baseline.'
    }

    $jsonResults = @($Json.results)
    if (([int]$Json.catalog_count -ne $script:ExpectedCatalogCount) -or
        ($jsonResults.Count -ne $script:ExpectedCatalogCount) -or
        ($Csv.Count -ne $script:ExpectedCatalogCount)) {
        throw "Expected $($script:ExpectedCatalogCount) records; JSON catalog=$($Json.catalog_count), JSON rows=$($jsonResults.Count), CSV rows=$($Csv.Count)."
    }

    if (([int]$Json.system.display -ne 2) -or
        ([int]$Json.system.gpu -ne 2) -or
        ([int]$Json.system.touch -ne 2) -or
        ([int]$Json.system.psram -ne 1)) {
        throw "System is not healthy: display=$($Json.system.display), gpu=$($Json.system.gpu), touch=$($Json.system.touch), psram=$($Json.system.psram)."
    }

    $byKey = @{}
    foreach ($result in $jsonResults) {
        $key = Get-ResultKey $result
        if ($byKey.ContainsKey($key)) {
            throw "Duplicate JSON result key '$key'."
        }
        if ($null -eq $result.checksum) {
            throw "JSON result '$key' has no checksum."
        }
        $computed = Get-ResultChecksum -Result $result -Protocol ([uint32]$Json.protocol)
        if ([uint32]$result.checksum -ne $computed) {
            throw "JSON result checksum mismatch for '$key'."
        }
        $byKey[$key] = $result
    }

    $commonFields = @(
        'state', 'error', 'sequence', 'iterations', 'elapsed_us', 'unit',
        'metric_x1000', 'minimum_x1000', 'maximum_x1000'
    )
    foreach ($row in $Csv) {
        $key = Get-ResultKey $row
        if (-not $byKey.ContainsKey($key)) {
            throw "CSV result '$key' is absent from JSON."
        }
        $peer = $byKey[$key]
        if ([string]$row.test_name -ne [string]$peer.name) {
            throw "CSV/JSON name mismatch for '$key'."
        }
        foreach ($field in $commonFields) {
            if ([string]$row.$field -ne [string]$peer.$field) {
                throw "CSV/JSON field '$field' mismatch for '$key'."
            }
        }
        if ([string]$row.result_checksum -ne [string]$peer.checksum) {
            throw "CSV/JSON checksum mismatch for '$key'."
        }
    }

    if ($RunNpu -or $RunAll) {
        foreach ($id in @(4, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                          16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                          26, 27, 28, 29, 30, 31, 32, 33, 34, 35)) {
            $key = "$id/u55"
            if ((-not $byKey.ContainsKey($key)) -or ($byKey[$key].state -ne 'complete')) {
                throw "Required NPU result '$key' is not complete."
            }
        }
    }
}

function Write-AtomicUtf8 {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Content
    )

    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
    try {
        [System.IO.File]::WriteAllText($temporary, $Content, $utf8WithoutBom)
        [System.IO.File]::Move($temporary, $Path)
    }
    finally {
        if ([System.IO.File]::Exists($temporary)) {
            [System.IO.File]::Delete($temporary)
        }
    }
}

$script:Serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One)
$script:Serial.ReadTimeout = 200
$script:Serial.WriteTimeout = 2000

try {
    $script:Serial.Open()
    Start-Sleep -Milliseconds 300
    $null = $script:Serial.ReadExisting()

    if ($RunNpu -and $RunAll) {
        throw 'Choose either -RunNpu or -RunAll; -RunAll already includes NPU.'
    }

    if ($RunAll) {
        Wait-BenchmarkCompletion -Command 'm55_coremark' -ResultKey '1/m55' `
            -CompletionTimeout 90
        Wait-SynchronousBenchmarkCompletion -Command 'm33_coremark' `
            -ResultKey '1/m33' -CompletionTimeout 90
        Wait-BenchmarkCompletion -Command 'm55_sram_bench' -ResultKey '2/m55'
        Wait-SynchronousBenchmarkCompletion -Command 'm33_sram_bench' -ResultKey '2/m33'
        Wait-BenchmarkCompletion -Command 'm55_sram_scalar' -ResultKey '5/m55'
        Wait-SynchronousBenchmarkCompletion -Command 'm33_sram_scalar' -ResultKey '5/m33'
        Wait-BenchmarkCompletion -Command 'm55_psram_scalar' -ResultKey '6/m55'
        Wait-SynchronousBenchmarkCompletion -Command 'm33_psram_scalar' `
            -ResultKey '6/m33' -CompletionTimeout 90
        Wait-SynchronousBenchmarkCompletion -Command 'ipc_bench' -ResultKey '3/m33'
    }

    if ($RunNpu -or $RunAll) {
        Wait-BenchmarkCompletion -Command 'npu_bench' -ResultKey '4/u55'
        Wait-BenchmarkCompletion -Command 'npu_mnist_bench' -ResultKey '10/u55'
        Wait-BenchmarkCompletion -Command 'npu_resnet_peak' -ResultKey '16/u55'
        Wait-BenchmarkCompletion -Command 'npu_mnist_peak' -ResultKey '21/u55'
        Wait-BenchmarkCompletion -Command 'npu_person_bench' -ResultKey '26/u55'
        Wait-BenchmarkCompletion -Command 'npu_person_peak' -ResultKey '31/u55'
    }

    $jsonSnapshot = Get-JsonSnapshot
    $csvSnapshot = Get-CsvSnapshot
    Assert-Reports -Json $jsonSnapshot.Data -Csv $csvSnapshot.Data

    if ($RunAll) {
        $incomplete = @($jsonSnapshot.Data.results | Where-Object { $_.state -ne 'complete' })
        if ($incomplete.Count -ne 0) {
            $keys = @($incomplete | ForEach-Object { Get-ResultKey $_ }) -join ', '
            throw "RunAll finished with non-COMPLETE results: $keys"
        }
    }

    [System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $jsonPath = Join-Path $OutputDirectory "benchmark-$stamp.json"
    $csvPath = Join-Path $OutputDirectory "benchmark-$stamp.csv"
    Write-AtomicUtf8 -Path $jsonPath -Content ($jsonSnapshot.Payload + [Environment]::NewLine)
    Write-AtomicUtf8 -Path $csvPath -Content ($csvSnapshot.Payload + [Environment]::NewLine)

    $states = @($jsonSnapshot.Data.results | Group-Object state | Sort-Object Name |
        ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ', '
    Write-Host "Validated schema=$($jsonSnapshot.Data.schema), ABI=$($jsonSnapshot.Data.status_abi), protocol=$($jsonSnapshot.Data.protocol), records=$($jsonSnapshot.Data.catalog_count)."
    Write-Host "States: $states"
    Write-Host "JSON: $jsonPath"
    Write-Host "CSV:  $csvPath"
}
finally {
    if (($null -ne $script:Serial) -and $script:Serial.IsOpen) {
        $script:Serial.Close()
    }
    if ($null -ne $script:Serial) {
        $script:Serial.Dispose()
    }
}
