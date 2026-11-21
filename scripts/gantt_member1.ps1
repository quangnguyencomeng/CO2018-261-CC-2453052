param(
    [string[]]$InputFiles = @(
        "output/sched.output",
        "output/sched_0.output",
        "output/sched_1.output",
        "output/os_1_singleCPU_mlq_paging.output"
    ),
    [string]$OutputFile = "docs/member1-gantt.md"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$lines = @(
    "# Member 1 - Scheduler Gantt",
    "",
    "Generated from scheduler logs. One time slot is represented by one unit.",
    ""
)

foreach ($inputFile in $InputFiles) {
    $path = Join-Path $repoRoot $inputFile
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Warning "Missing log: $inputFile"
        continue
    }

    $currentSlot = 0
    $currentByCpu = @{}
    $intervals = @{}
    $maxSlot = 0

    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -match "Time slot\s+(\d+)") {
            $currentSlot = [int]$matches[1]
            if ($currentSlot -gt $maxSlot) {
                $maxSlot = $currentSlot
            }
            continue
        }

        if ($line -match "CPU\s+(\d+):\s+Dispatched process\s+(\d+)") {
            $cpu = [int]$matches[1]
            $processId = [int]$matches[2]
            $currentByCpu[$cpu] = [PSCustomObject]@{
                Pid = $processId
                Start = $currentSlot
            }
            continue
        }

        if ($line -match "CPU\s+(\d+):\s+(?:Put process|Processed)\s+(\d+)") {
            $cpu = [int]$matches[1]
            $processId = [int]$matches[2]
            if (-not $intervals.ContainsKey($cpu)) {
                $intervals[$cpu] = @()
            }
            if ($currentByCpu.ContainsKey($cpu)) {
                $start = $currentByCpu[$cpu].Start
                if ($currentByCpu[$cpu].Pid -eq $processId -and $currentSlot -gt $start) {
                    $intervals[$cpu] += [PSCustomObject]@{
                        Pid = $processId
                        Start = $start
                        End = $currentSlot
                    }
                }
            }
            $currentByCpu[$cpu] = $null
            continue
        }
    }

    foreach ($cpu in $currentByCpu.Keys) {
        if ($null -ne $currentByCpu[$cpu]) {
            $start = $currentByCpu[$cpu].Start
            $processId = $currentByCpu[$cpu].Pid
            if (-not $intervals.ContainsKey($cpu)) {
                $intervals[$cpu] = @()
            }
            if ($maxSlot -ge $start) {
                $intervals[$cpu] += [PSCustomObject]@{
                    Pid = $processId
                    Start = $start
                    End = $maxSlot + 1
                }
            }
        }
    }

    $lines += "## $([System.IO.Path]::GetFileNameWithoutExtension($inputFile))"
    $lines += ""
    $lines += '```mermaid'
    $lines += "gantt"
    $lines += "  title Scheduler Gantt"
    $lines += "  dateFormat X"
    $lines += "  axisFormat %M:%S"

    $cpus = @($intervals.Keys | Sort-Object)
    foreach ($cpu in $cpus) {
        $lines += "  section CPU $cpu"
        foreach ($interval in $intervals[$cpu]) {
            $duration = $interval.End - $interval.Start
            if ($duration -gt 0) {
                $lines += "  PID $($interval.Pid) :$($interval.Start),${duration}s"
            }
        }
    }

    $lines += '```'
    $lines += ""
}

$outputPath = Join-Path $repoRoot $OutputFile
$outputDir = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir | Out-Null
}
$lines | Set-Content -LiteralPath $outputPath -Encoding UTF8
Write-Output "Generated $OutputFile"
