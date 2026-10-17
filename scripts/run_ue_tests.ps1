#Requires -Version 5.1
<#
.SYNOPSIS
    Task 8 — headless Unreal Automation test runner for Sensor Sim Sandbox
    (Requirements 9.1, 9.2, 9.6).

.DESCRIPTION
    Builds the editor target, then runs every project Unreal Automation test
    headless and exits 0 ONLY when all of them pass and at least one ran. All
    project tests are registered under the `SensorSimSandbox.` prefix
    (SensorSimSandbox.Pipeline.*, .Rover.*, .Lidar.*, .Logging.*), so the suite
    name `SensorSimSandbox` selects exactly the project's own tests.

    Two steps, non-interactive and host-run (one Unreal process at a time, long
    steps polled; the UBT Makefile.bin cache is cleared first so newly added
    test source files are always picked up — the same cache quirk the task 6
    scripts hit):

      1. Build the editor target with Build.bat ... -WaitMutex (so no "rebuild
         modules" prompt can appear on the later -game launch); compile log
         saved. On this host the editor is already built, so this is incremental.

      2. Headless automation run — the exact invocation the design fixes:

           UnrealEditor-Cmd.exe <uproject>
             -ExecCmds="Automation RunTests SensorSimSandbox;Quit"
             -unattended -nopause -nosplash -nullrhi
             -testexit="Automation Test Queue Empty"
             -ReportExportPath=<runs/logs/ue_tests>
             -log

         The run writes a JSON report (index.json) to -ReportExportPath. The
         script parses it and applies the design's exact pass condition:

             exit 0  ONLY IF  failed == 0  AND  succeeded >= 1  AND  notRun == 0

         Any of the following exits NON-ZERO:
           * a build failure,
           * a zero-tests-found result,
           * one or more failed tests,
           * any skipped / not-run tests,
           * a missing or garbled report.

    Raw logs and the exported report live under runs/logs/ue_tests/ (gitignored).

.PARAMETER EnginePath
    UE 5.8 install root. Default is the verified path on this host so the M1
    gate can call the script with no arguments; override it to point at another
    engine install.

.PARAMETER BuildTimeoutSec
    Max seconds to wait for the editor build. Default 1800 (30 min).

.PARAMETER TestTimeoutSec
    Max seconds to wait for the automation run. Default 1800 (30 min).

.PARAMETER TestSuite
    Automation suite prefix to run. Default `SensorSimSandbox` (all project
    tests). Overridable only to scope a subset while debugging; the gate uses
    the default so every project test runs.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_ue_tests.ps1
#>

[CmdletBinding()]
param(
    [string]$EnginePath = 'C:\Program Files (x86)\UE_5.8',
    [int]$BuildTimeoutSec = 1800,
    [int]$TestTimeoutSec = 1800,
    [string]$TestSuite = 'SensorSimSandbox'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$RepoRoot   = Split-Path -Parent $PSScriptRoot
$UProject   = Join-Path $RepoRoot 'unreal\SensorSimSandbox\SensorSimSandbox.uproject'
$LogDir     = Join-Path $RepoRoot 'runs\logs\ue_tests'
$ReportDir  = Join-Path $LogDir 'report'
New-Item -ItemType Directory -Force -Path $LogDir  | Out-Null
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null

$BuildBat   = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
$EditorCmd  = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'

function Write-Step($msg) { Write-Host "[run_ue_tests] $msg" }

# ---------------------------------------------------------------------------
# 0. Clear the UBT Makefile.bin cache so newly added test .cpp files are always
#    picked up (prior tasks hit a stale cache that silently dropped new source).
# ---------------------------------------------------------------------------
function Clear-UbtCache {
    $intermediate = Join-Path $RepoRoot 'unreal\SensorSimSandbox\Intermediate\Build'
    if (Test-Path $intermediate) {
        Get-ChildItem -Path $intermediate -Recurse -Filter 'Makefile.bin' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        Write-Step "cleared UBT Makefile.bin cache under $intermediate"
    }
}

# ---------------------------------------------------------------------------
# Only one Unreal process at a time.
# ---------------------------------------------------------------------------
function Wait-ForNoUnrealProcess {
    for ($i = 0; $i -lt 60; $i++) {
        $busy = Get-Process -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessName -match 'UnrealBuildTool|UnrealEditor' }
        if (-not $busy) { return }
        Start-Sleep -Seconds 2
    }
}

# ---------------------------------------------------------------------------
# 1. Build the editor target (background + poll).
# ---------------------------------------------------------------------------
function Invoke-Build {
    $compileLog = Join-Path $LogDir 'compile.log'
    if (-not (Test-Path $BuildBat)) {
        "Build.bat not found at $BuildBat" | Set-Content -Path $compileLog -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = "Build.bat not found at $BuildBat"; Log = $compileLog }
    }
    if (-not (Test-Path $UProject)) {
        "uproject not found at $UProject" | Set-Content -Path $compileLog -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = "uproject not found at $UProject"; Log = $compileLog }
    }

    Wait-ForNoUnrealProcess

    Write-Step "Building SensorSimSandboxEditor (log: $compileLog) ..."
    $buildArgs = @(
        'SensorSimSandboxEditor', 'Win64', 'Development',
        "-Project=`"$UProject`"", '-WaitMutex', '-NoHotReloadFromIDE'
    )
    $p = Start-Process -FilePath $BuildBat -ArgumentList $buildArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput $compileLog -RedirectStandardError "$compileLog.err"
    if (-not $p.WaitForExit($BuildTimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        return [pscustomobject]@{ Ok = $false; Detail = 'build timed out'; Log = $compileLog }
    }

    # Build.bat shells out to UnrealBuildTool, so the PassThru process's
    # ExitCode is not always populated when WaitForExit(timeout) returns. Treat
    # the authoritative signal as UBT's own "Result:" line in the log, falling
    # back to a usable ExitCode. Success requires an explicit "Result: Succeeded"
    # (or a zero exit code with no "Result: Failed").
    $exitCode = $null
    try { $exitCode = $p.ExitCode } catch { $exitCode = $null }
    $logText = if (Test-Path $compileLog) { Get-Content $compileLog -Raw } else { '' }
    $sawSucceeded = ($logText -match '(?m)^\s*Result:\s*Succeeded\s*$')
    $sawFailed    = ($logText -match '(?m)^\s*Result:\s*Failed') -or ($logText -match '(?m)^\s*BUILD FAILED\s*$')

    if ($sawFailed) {
        return [pscustomobject]@{ Ok = $false; Detail = "Build.bat reported Result: Failed (exit $exitCode)"; Log = $compileLog }
    }
    $ok = $sawSucceeded -or ($exitCode -eq 0)
    $detail = if ($sawSucceeded) { "Build.bat Result: Succeeded (exit $exitCode)" } else { "Build.bat exit $exitCode" }
    return [pscustomobject]@{ Ok = $ok; Detail = $detail; Log = $compileLog }
}

# ---------------------------------------------------------------------------
# 2. Headless automation run with the design's exact invocation.
# ---------------------------------------------------------------------------
function Invoke-AutomationRun {
    $runLog = Join-Path $LogDir 'automation.log'
    if (-not (Test-Path $EditorCmd)) {
        "UnrealEditor-Cmd.exe not found at $EditorCmd" | Set-Content -Path $runLog -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = "UnrealEditor-Cmd.exe not found at $EditorCmd"; Log = $runLog }
    }

    # Start from a clean report dir so a stale index.json can never be read as
    # this run's result.
    if (Test-Path $ReportDir) { Remove-Item -Path $ReportDir -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null

    Wait-ForNoUnrealProcess

    Write-Step "Running headless automation suite '$TestSuite' (log: $runLog) ..."
    $execCmds = "Automation RunTests $TestSuite;Quit"
    $runArgs = @(
        "`"$UProject`"",
        "-ExecCmds=`"$execCmds`"",
        '-unattended', '-nopause', '-nosplash', '-nullrhi',
        '-testexit="Automation Test Queue Empty"',
        "-ReportExportPath=`"$ReportDir`"",
        '-log',
        "-abslog=`"$runLog`""
    )
    $p = Start-Process -FilePath $EditorCmd -ArgumentList $runArgs -PassThru
    if (-not $p.WaitForExit($TestTimeoutSec * 1000)) {
        try { if (-not $p.HasExited) { $p.Kill() } } catch { }
        return [pscustomobject]@{ Ok = $false; Detail = 'automation run timed out'; Log = $runLog }
    }
    return [pscustomobject]@{ Ok = $true; Detail = "UnrealEditor-Cmd exit $($p.ExitCode)"; Log = $runLog }
}

# ---------------------------------------------------------------------------
# Parse the exported report (index.json). The report has a top-level `tests`
# array; each entry carries a `state` ("Success"/"Fail"/"NotRun"/"Skipped") and
# an `errors` count. We also honour an aggregate `succeeded`/`failed`/`notRun`
# block when present. Returns per-state counts and a `Parsed` flag; a missing or
# garbled report sets Parsed=$false (a non-zero exit).
# ---------------------------------------------------------------------------
function Read-Report {
    $index = Join-Path $ReportDir 'index.json'
    $result = [pscustomobject]@{
        Parsed    = $false
        Total     = 0
        Succeeded = 0
        Failed    = 0
        NotRun    = 0
        Detail    = ''
        Path      = $index
    }

    if (-not (Test-Path $index)) {
        $result.Detail = "report index.json not found at $index"
        return $result
    }

    $raw = ''
    try { $raw = Get-Content -Path $index -Raw -ErrorAction Stop } catch {
        $result.Detail = "could not read report: $($_.Exception.Message)"
        return $result
    }
    if ([string]::IsNullOrWhiteSpace($raw)) {
        $result.Detail = 'report index.json is empty'
        return $result
    }

    $json = $null
    try { $json = $raw | ConvertFrom-Json -ErrorAction Stop } catch {
        $result.Detail = "report index.json is not valid JSON: $($_.Exception.Message)"
        return $result
    }

    $succeeded = 0; $failed = 0; $notRun = 0; $total = 0

    # Prefer counting each test's own state so skipped/not-run tests are caught
    # even when an aggregate block is absent.
    $tests = $null
    if ($json.PSObject.Properties.Name -contains 'tests') { $tests = $json.tests }
    if ($null -ne $tests) {
        foreach ($t in @($tests)) {
            $total++
            $state = ''
            if ($t.PSObject.Properties.Name -contains 'state') { $state = [string]$t.state }
            $errCount = 0
            if ($t.PSObject.Properties.Name -contains 'errors') {
                try { $errCount = [int]$t.errors } catch { $errCount = 0 }
            }
            switch -Regex ($state) {
                '^(?i)Success$' {
                    # A "Success" state with recorded errors is still a failure.
                    if ($errCount -gt 0) { $failed++ } else { $succeeded++ }
                }
                '^(?i)Fail' { $failed++ }
                '^(?i)(NotRun|Skipped|Excluded)$' { $notRun++ }
                default {
                    # Unknown/blank state is treated as not-run (conservative).
                    $notRun++
                }
            }
        }
        $result.Parsed = $true
    }
    elseif (($json.PSObject.Properties.Name -contains 'succeeded') -or
            ($json.PSObject.Properties.Name -contains 'failed')) {
        # Aggregate-only report shape.
        if ($json.PSObject.Properties.Name -contains 'succeeded') { try { $succeeded = [int]$json.succeeded } catch { } }
        if ($json.PSObject.Properties.Name -contains 'failed')    { try { $failed    = [int]$json.failed }    catch { } }
        if ($json.PSObject.Properties.Name -contains 'notRun')    { try { $notRun    = [int]$json.notRun }    catch { } }
        $total = $succeeded + $failed + $notRun
        $result.Parsed = $true
    }
    else {
        $result.Detail = 'report index.json has neither a tests array nor succeeded/failed counts'
        return $result
    }

    $result.Total     = $total
    $result.Succeeded = $succeeded
    $result.Failed    = $failed
    $result.NotRun    = $notRun
    $result.Detail    = "total=$total succeeded=$succeeded failed=$failed notRun=$notRun"
    return $result
}

# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------
Clear-UbtCache

$build = Invoke-Build
Write-Step "Build: Ok=$($build.Ok) $($build.Detail)"
if (-not $build.Ok) {
    Write-Error "FAIL: editor build failed; see $($build.Log)"
    exit 1
}

$run = Invoke-AutomationRun
Write-Step "Automation run: $($run.Detail)"
if (-not $run.Ok) {
    Write-Error "FAIL: automation run did not complete; see $($run.Log)"
    exit 1
}

$report = Read-Report
Write-Step "Report: $($report.Detail)"

if (-not $report.Parsed) {
    Write-Error "FAIL: could not parse the automation report ($($report.Detail)). See $($report.Path) and $($run.Log)."
    exit 1
}

# ---------------------------------------------------------------------------
# Pass condition (design): exit 0 ONLY IF failed == 0 AND succeeded >= 1 AND
# notRun == 0.
# ---------------------------------------------------------------------------
$pass = ($report.Failed -eq 0) -and ($report.Succeeded -ge 1) -and ($report.NotRun -eq 0)

Write-Host ''
Write-Host '==== SensorSimSandbox Unreal Automation summary ====' -ForegroundColor Cyan
Write-Host ("  tests run : {0}" -f $report.Total)
Write-Host ("  succeeded : {0}" -f $report.Succeeded)
Write-Host ("  failed    : {0}" -f $report.Failed)
Write-Host ("  not run   : {0}" -f $report.NotRun)
Write-Host ("  report    : {0}" -f $report.Path)
Write-Host ("  log       : {0}" -f $run.Log)

if ($pass) {
    Write-Host 'RESULT: PASS - all tests passed and at least one ran.' -ForegroundColor Green
    exit 0
}

$reasons = @()
if ($report.Succeeded -lt 1) { $reasons += 'no tests ran (zero successes)' }
if ($report.Failed -gt 0)    { $reasons += "$($report.Failed) test(s) failed" }
if ($report.NotRun -gt 0)    { $reasons += "$($report.NotRun) test(s) did not run / were skipped" }
Write-Host ("RESULT: FAIL - {0}." -f ($reasons -join '; ')) -ForegroundColor Red
exit 1
