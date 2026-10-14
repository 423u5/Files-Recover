# Reads the JUnit file `ctest --output-junit` wrote and reports every failed
# test as a GitHub error annotation, with the lines of its output that say
# why. Annotations can be read by anyone who can see the repository (also
# through the API), unlike the job's log, which needs admin rights.
#
# Usage: report_failed_tests.ps1 -Junit <file> [-Preset <name>] [-MaxAnnotations 10]
param(
    [Parameter(Mandatory = $true)][string]$Junit,
    [string]$Preset = "",
    [int]$MaxAnnotations = 10,
    [int]$MaxLines = 40
)

$ErrorActionPreference = "Stop"

function Escape-Annotation([string]$text) {
    # Workflow commands use % escapes for these characters.
    return $text.Replace("%", "%25").Replace("`r", "%0D").Replace("`n", "%0A")
}

function Escape-Property([string]$text) {
    return (Escape-Annotation $text).Replace(":", "%3A").Replace(",", "%2C")
}

# The lines of a test's output that explain a failure: GoogleTest's failure
# lines and what follows them, and AddressSanitizer's report.
function Select-Reason([string]$output, [int]$maxLines) {
    $lines = $output -split "`r?`n"
    $picked = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $lines.Count -and $picked.Count -lt $maxLines; $i++) {
        $line = $lines[$i]
        if ($line -match ": error: |^\[  FAILED  \]|Failure$|ERROR: AddressSanitizer|SUMMARY: AddressSanitizer|exception|Exception") {
            $picked.Add($line)
            for ($j = $i + 1; $j -lt $lines.Count -and $j -le $i + 8 -and $picked.Count -lt $maxLines; $j++) {
                if ($lines[$j] -match "^\[ +(RUN|OK|FAILED|PASSED|SKIPPED) +\]") { break }
                $picked.Add($lines[$j])
            }
        }
    }
    if ($picked.Count -eq 0) {
        # No recognisable failure line (a crash, a timeout): the end of the output.
        $tail = $lines | Where-Object { $_.Trim().Length -gt 0 } | Select-Object -Last $maxLines
        foreach ($line in $tail) { $picked.Add($line) }
    }
    return ($picked -join "`n")
}

if (-not (Test-Path -LiteralPath $Junit)) {
    Write-Output "::warning title=No test results::$(Escape-Annotation "ctest wrote no JUnit file at $Junit (the tests did not run?)")"
    exit 0
}

[xml]$document = Get-Content -LiteralPath $Junit -Raw -Encoding UTF8
$failed = @()
foreach ($case in $document.SelectNodes("//testcase")) {
    $status = $case.GetAttribute("status")
    $hasFailure = $null -ne $case.SelectSingleNode("failure") -or $null -ne $case.SelectSingleNode("error")
    if ($hasFailure -or $status -eq "fail") {
        $failed += $case
    }
}

$summary = $env:GITHUB_STEP_SUMMARY
if ($failed.Count -eq 0) {
    Write-Output "No failed tests in $Junit."
    if ($summary) { Add-Content -LiteralPath $summary -Value "No failed tests ($Preset)." -Encoding UTF8 }
    exit 0
}

Write-Output "$($failed.Count) failed test(s) in $Junit."
if ($summary) {
    Add-Content -LiteralPath $summary -Value "### $($failed.Count) failed test(s) ($Preset)`n" -Encoding UTF8
}
$count = 0
foreach ($case in $failed) {
    $name = $case.GetAttribute("name")
    $output = ""
    $out = $case.SelectSingleNode("system-out")
    if ($null -ne $out) { $output = $out.InnerText }
    $failure = $case.SelectSingleNode("failure")
    $message = ""
    if ($null -ne $failure) { $message = $failure.GetAttribute("message") }
    $reason = Select-Reason $output $MaxLines
    Write-Output "---- $name ($message)"
    Write-Output $reason
    if ($summary) {
        Add-Content -LiteralPath $summary -Value "- ``$name`` ($message)" -Encoding UTF8
    }
    if ($count -lt $MaxAnnotations) {
        $title = Escape-Property "$Preset failed: $name"
        Write-Output "::error title=$title::$(Escape-Annotation "$name ($message)`n$reason")"
    }
    $count++
}
if ($count -gt $MaxAnnotations) {
    Write-Output "::error title=$(Escape-Property "$Preset")::$(Escape-Annotation "$($count - $MaxAnnotations) more failed tests: see the step summary")"
}
exit 0
