# Runs the fuzz targets (fuzz/, preset msvc-fuzz) for a time budget each, a
# few at once, and reports what they found. Run it from a developer prompt (or
# after vcvars64.bat): the targets need AddressSanitizer's runtime DLL.
#
#   powershell -File fuzz\run_fuzzers.ps1 -Seeds <seeds> -Work <work> [-Seconds 600] [-Jobs 3] [-Targets a,b]
#
# <seeds> is what `test_image_generator fuzz-seeds --output <seeds>` wrote.
# Each target's corpus grows in <work>\corpus\<target> (reused by later runs),
# its log is <work>\logs\<target>.log, and any input that fails it is kept in
# <work>\artifacts\<target>\ (crash-*, leak-*, timeout-*, oom-*). The script
# exits 1 when any target left an artifact or failed to run.
param(
    [Parameter(Mandatory = $true)][string]$Seeds,
    [Parameter(Mandatory = $true)][string]$Work,
    [int]$Seconds = 600,
    [int]$Jobs = 3,
    [string[]]$Targets = @("partition", "filesystem", "formats", "media", "mp4", "metadata", "journal",
                           "imgmeta", "carving", "scan"),
    # The fuzz targets; default: build\msvc-fuzz\fuzz of this checkout.
    [string]$Build = ""
)

$ErrorActionPreference = "Stop"
if (-not $Build) { $Build = Join-Path $PSScriptRoot "..\build\msvc-fuzz\fuzz" }
$Build = (Resolve-Path $Build).Path
New-Item -ItemType Directory -Force -Path (Join-Path $Work "logs") | Out-Null

$pending = [System.Collections.Generic.Queue[string]]::new()
foreach ($target in $Targets) { $pending.Enqueue($target) }
$running = @{}
$failed = @()

function Start-Target([string]$target) {
    $exe = Join-Path $Build "fuzz_$target.exe"
    if (-not (Test-Path $exe)) { throw "no fuzz target $exe (build the msvc-fuzz preset)" }
    $corpus = Join-Path $Work "corpus\$target"
    $artifacts = Join-Path $Work "artifacts\$target"
    New-Item -ItemType Directory -Force -Path $corpus, $artifacts | Out-Null
    $arguments = @("`"$corpus`"")
    $seedDir = Join-Path $Seeds $target
    if (Test-Path $seedDir) { $arguments += "`"$seedDir`"" }
    # Forward slashes: a backslash before the closing quote would escape it.
    $prefix = ($artifacts -replace '\\', '/') + '/'
    $arguments += @("-max_total_time=$Seconds", "-rss_limit_mb=3072", "-timeout=30", "-print_final_stats=1",
                    "-artifact_prefix=`"$prefix`"")
    # Inputs a whole scan or carving pass takes in stride: larger ones only slow the search down.
    $maxLen = @{ "scan" = 262144; "carving" = 262144 }
    if ($maxLen.ContainsKey($target)) { $arguments += "-max_len=$($maxLen[$target])" }
    $log = Join-Path $Work "logs\$target.log"
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput (Join-Path $Work "logs\$target.out")
    $null = $process.Handle  # keeps the handle, so that ExitCode is known once it exits
    Write-Output "started $target (pid $($process.Id))"
    return $process
}

while ($pending.Count -gt 0 -or $running.Count -gt 0) {
    while ($running.Count -lt $Jobs -and $pending.Count -gt 0) {
        $target = $pending.Dequeue()
        $running[$target] = Start-Target $target
    }
    Start-Sleep -Seconds 5
    foreach ($target in @($running.Keys)) {
        $process = $running[$target]
        if ($process.HasExited) {
            $running.Remove($target)
            $artifacts = @(Get-ChildItem (Join-Path $Work "artifacts\$target") -File -ErrorAction SilentlyContinue)
            $stats = Select-String -Path (Join-Path $Work "logs\$target.log") -Pattern "stat::number_of_executed_units|stat::peak_rss_mb|cov: " |
                Select-Object -Last 3 | ForEach-Object { $_.Line.Trim() }
            Write-Output ("finished {0}: exit {1}, {2} artifact(s); {3}" -f $target, $process.ExitCode, $artifacts.Count,
                ($stats -join "; "))
            if ($artifacts.Count -gt 0 -or $process.ExitCode -ne 0) { $failed += $target }
        }
    }
}

if ($failed.Count -gt 0) {
    Write-Output "targets with findings or failures: $($failed -join ', ')"
    exit 1
}
Write-Output "no findings"
exit 0
