#Requires -Version 5.1
<#
.SYNOPSIS
    Task 14.1 [HOST] demo/verify — build a sensor_msgs/CompressedImage (JPEG) and
    an accompanying sensor_msgs/CameraInfo from a real camera frame, publish both
    through the Rosbridge_Client (same stamp) to /camera/image/compressed and
    /camera/camera_info, then confirm them on ROS 2 topics (Requirements 4.3, 4.4).

.DESCRIPTION
    Mirrors scripts/run_lidar_publish.ps1 exactly (world-needing console command
    via a -game launch; rosbridge-in-WSL with url_path:=// per the M0 finding).
    Non-interactive, host-run, one Unreal process at a time, long steps polled.

      1. Build the editor target with Build.bat ... -WaitMutex; compile log saved.
      2. Ensure rosbridge answers an HTTP 101 WebSocket upgrade at url_path '//'
         in WSL (reuses a persistent session if already up; starts it otherwise).
      3. Start one-shot `ros2 topic echo --once` subscribers for
         /camera/camera_info AND /camera/image/compressed in WSL BEFORE
         publishing (the publish is a single shot), writing to files on /mnt/c.
      4. Launch /Game/Maps/RoverLevel with -game (a live world + GPU render is
         required for the scene capture) and -ExecCmds="CameraPublish <cfg>".
         CameraPublish spawns a lit scene + camera, captures one real frame,
         JPEG-encodes the published RGBA, builds the CompressedImage + CameraInfo
         (FCameraMessage) with the SAME stamp, publishes both through the live
         Rosbridge_Client, logs the built format/jpeg size/intrinsics, then
         self-terminates.
      5. Confirm the echoed CameraInfo shows width/height == config (640x480),
         and base64-decode the echoed CompressedImage data to a JPEG whose
         dimensions == config. Save the outputs as evidence. The echo is NEVER
         faked; if rosbridge/WSL is unreachable the real step is DEFERRED with the
         reason, and the headless message/JPEG/intrinsics verification
         (SensorSimSandbox.Camera.MessageBuildDecode) stands as the authoritative
         logic check.

.PARAMETER EnginePath
    UE 5.8 install root. Default is the verified path on this host.

.PARAMETER WslDistro
    WSL distro hosting ROS 2 Jazzy. Default 'Ubuntu'.

.EXAMPLE
    pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_camera_publish.ps1
#>

[CmdletBinding()]
param(
    [string]$EnginePath = 'C:\Program Files (x86)\UE_5.8',
    [string]$WslDistro = 'Ubuntu',
    [int]$RealPort = 9090,
    [int]$BuildTimeoutSec = 1800,
    [int]$LaunchTimeoutSec = 900,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot    = Split-Path -Parent $PSScriptRoot
$UProject    = Join-Path $RepoRoot 'unreal\SensorSimSandbox\SensorSimSandbox.uproject'
$CameraCfg   = Join-Path $RepoRoot 'config\camera.yaml'
$BuildBat    = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
$EditorExe   = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor.exe'
$LogDir      = Join-Path $RepoRoot 'runs\logs\camera_publish'
$EvidenceDir = Join-Path $RepoRoot 'docs\evidence'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

function Write-Step($m) { Write-Host "[run_camera_publish] $m" }

function Clear-UbtCache {
    $intermediate = Join-Path $RepoRoot 'unreal\SensorSimSandbox\Intermediate\Build'
    if (Test-Path $intermediate) {
        Get-ChildItem -Path $intermediate -Recurse -Filter 'Makefile.bin' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        Write-Step "cleared UBT Makefile.bin cache under $intermediate"
    }
}

function Wait-ForNoUnrealProcess {
    for ($i = 0; $i -lt 60; $i++) {
        $busy = Get-Process -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessName -match 'UnrealBuildTool|UnrealEditor' }
        if (-not $busy) { return }
        Start-Sleep -Seconds 2
    }
}

function Invoke-Wsl {
    param([Parameter(Mandatory)][string]$BashCommand)
    $full = "source /opt/ros/jazzy/setup.bash 2>/dev/null; $BashCommand"
    $result = [pscustomobject]@{ Ok = $false; Output = '' }
    try {
        $out = & wsl.exe -d $WslDistro -- bash -lc "$full" 2>&1 | Out-String
        $result.Output = $out
        $result.Ok = ($LASTEXITCODE -eq 0)
    } catch { $result.Output = $_.Exception.Message }
    return $result
}

function Test-WslAvailable {
    try { $null = & wsl.exe -l -q 2>$null; return ($LASTEXITCODE -eq 0) } catch { return $false }
}

function Convert-ToWslPath {
    param([Parameter(Mandatory)][string]$WinPath)
    $p = $WinPath -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') { return "/mnt/" + $Matches[1].ToLower() + $Matches[2] }
    return $p
}

function Test-WsUpgrade {
    param([string]$HostName = '127.0.0.1', [int]$Port = 9090, [string]$Path = '//')
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect($HostName, $Port)
        $stream = $client.GetStream()
        $key = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes((Get-Random).ToString()))
        $req = "GET $Path HTTP/1.1`r`nHost: $HostName`:$Port`r`nUpgrade: websocket`r`nConnection: Upgrade`r`nSec-WebSocket-Key: $key`r`nSec-WebSocket-Version: 13`r`n`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($req)
        $stream.Write($bytes, 0, $bytes.Length)
        Start-Sleep -Milliseconds 300
        $buf = New-Object byte[] 1024
        $n = $stream.Read($buf, 0, $buf.Length)
        $line = (([Text.Encoding]::ASCII.GetString($buf, 0, $n)) -split "`r`n")[0]
        $client.Close()
        return ($line -match '101')
    } catch { return $false }
}

function Ensure-Rosbridge {
    param([int]$Port = 9090, [string]$UrlPath = '//')
    if (Test-WsUpgrade -Port $Port -Path $UrlPath) {
        return [pscustomobject]@{ Ok = $true; Detail = "rosbridge already answering HTTP 101 on localhost:$Port at '$UrlPath'." }
    }
    $clean = "pkill -9 -f rosbridge_websocket 2>/dev/null; pkill -9 -f 'ros2 launch rosbridge' 2>/dev/null; pkill -9 -f rosapi_node 2>/dev/null; fuser -k $Port/tcp 2>/dev/null; sleep 2; echo cleaned"
    $null = Invoke-Wsl -BashCommand $clean
    $launch = "nohup ros2 run rosbridge_server rosbridge_websocket --ros-args -p url_path:=$UrlPath > /tmp/rosbridge_task14.log 2>&1 & disown; echo started"
    $null = Invoke-Wsl -BashCommand $launch
    for ($i = 0; $i -lt 25; $i++) {
        Start-Sleep -Seconds 1
        if (Test-WsUpgrade -Port $Port -Path $UrlPath) {
            return [pscustomobject]@{ Ok = $true; Detail = "rosbridge started in WSL (url_path=$UrlPath), HTTP 101 on localhost:$Port." }
        }
    }
    return [pscustomobject]@{ Ok = $false; Detail = "rosbridge did not answer HTTP 101 on localhost:$Port at '$UrlPath' within timeout." }
}

function Invoke-Build {
    $compileLog = Join-Path $LogDir 'compile.log'
    if (-not (Test-Path $BuildBat)) {
        return [pscustomobject]@{ Ok = $false; Detail = "Build.bat not found at $BuildBat"; Log = $compileLog }
    }
    Wait-ForNoUnrealProcess
    Write-Step "Building SensorSimSandboxEditor (log: $compileLog) ..."
    $buildArgs = @('SensorSimSandboxEditor', 'Win64', 'Development', "-Project=`"$UProject`"", '-WaitMutex', '-NoHotReloadFromIDE')
    $p = Start-Process -FilePath $BuildBat -ArgumentList $buildArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput $compileLog -RedirectStandardError "$compileLog.err"
    if (-not $p.WaitForExit($BuildTimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        return [pscustomobject]@{ Ok = $false; Detail = 'build timed out'; Log = $compileLog }
    }
    $logText = if (Test-Path $compileLog) { Get-Content $compileLog -Raw } else { '' }
    $sawSucceeded = ($logText -match '(?m)^\s*Result:\s*Succeeded\s*$')
    $sawFailed    = ($logText -match '(?m)^\s*Result:\s*Failed') -or ($logText -match '(?m)^\s*BUILD FAILED\s*$')
    $exitCode = $null; try { $exitCode = $p.ExitCode } catch { $exitCode = $null }
    if ($sawFailed) { return [pscustomobject]@{ Ok = $false; Detail = "Result: Failed (exit $exitCode)"; Log = $compileLog } }
    $ok = $sawSucceeded -or ($exitCode -eq 0)
    return [pscustomobject]@{ Ok = $ok; Detail = "Result: $(if($sawSucceeded){'Succeeded'}else{"exit $exitCode"})"; Log = $compileLog }
}

function Invoke-Publish {
    $editorLog = Join-Path $LogDir 'camera_publish_editor.log'
    $infoFile  = Join-Path $LogDir 'ros2_echo_camera_info.txt'
    $imageFile = Join-Path $LogDir 'ros2_echo_camera_image.txt'
    $jpegSize  = Join-Path $LogDir 'decoded_jpeg_size.txt'
    foreach ($f in @($infoFile, $imageFile, $jpegSize)) { if (Test-Path $f) { Remove-Item $f -Force } }

    $deferredReason = $null
    if (-not (Test-WslAvailable)) {
        $deferredReason = 'WSL not available on this host'
    } else {
        $rb = Ensure-Rosbridge -Port $RealPort -UrlPath '//'
        Write-Step "rosbridge: Ok=$($rb.Ok) $($rb.Detail)"
        if (-not $rb.Ok) { $deferredReason = "rosbridge not reachable: $($rb.Detail)" }
    }

    $infoProc = $null
    $imageProc = $null
    if (-not $deferredReason) {
        # One-shot echo subscribers started BEFORE publishing. CameraInfo with
        # --no-arr keeps K/P readable; the image with --no-arr would hide the
        # base64 data we need, so the image echo keeps the full message.
        $wslInfo = Convert-ToWslPath $infoFile
        $infoHelper = Join-Path $LogDir 'echo_info.sh'
        $infoBody = "#!/usr/bin/env bash`nset -e`nsource /opt/ros/jazzy/setup.bash`nexec stdbuf -oL -eL ros2 topic echo --once /camera/camera_info sensor_msgs/msg/CameraInfo > `"`$1`" 2>&1`n"
        [IO.File]::WriteAllText($infoHelper, ($infoBody -replace "`r`n", "`n"))
        $wslInfoHelper = Convert-ToWslPath $infoHelper
        $infoProc = Start-Process -FilePath 'wsl.exe' `
            -ArgumentList @('-d', $WslDistro, '--', 'bash', $wslInfoHelper, $wslInfo) -PassThru -WindowStyle Hidden

        $wslImage = Convert-ToWslPath $imageFile
        $imageHelper = Join-Path $LogDir 'echo_image.sh'
        # --full-length so the complete JPEG byte array is printed (ros2 echo
        # truncates long arrays to ~128 entries by default, which would cut off
        # the SOF marker carrying the image dimensions).
        $imageBody = "#!/usr/bin/env bash`nset -e`nsource /opt/ros/jazzy/setup.bash`nexec stdbuf -oL -eL ros2 topic echo --once --full-length /camera/image/compressed sensor_msgs/msg/CompressedImage > `"`$1`" 2>&1`n"
        [IO.File]::WriteAllText($imageHelper, ($imageBody -replace "`r`n", "`n"))
        $wslImageHelper = Convert-ToWslPath $imageHelper
        $imageProc = Start-Process -FilePath 'wsl.exe' `
            -ArgumentList @('-d', $WslDistro, '--', 'bash', $wslImageHelper, $wslImage) -PassThru -WindowStyle Hidden

        Start-Sleep -Seconds 6
    }

    Wait-ForNoUnrealProcess
    Write-Step "Launching -game RoverLevel CameraPublish (log: $editorLog) ..."
    $execCmds = "CameraPublish `"$CameraCfg`""
    $editorArgs = @(
        "`"$UProject`"", '/Game/Maps/RoverLevel', '-game',
        '-ResX=640', '-ResY=480', '-windowed',
        "-BridgeHost=127.0.0.1", "-BridgePort=$RealPort",
        "-ExecCmds=`"$execCmds`"",
        '-stdout', '-FullStdOutLogOutput',
        '-unattended', '-nopause', '-nosplash',
        "-abslog=`"$editorLog`""
    )
    $p = Start-Process -FilePath $EditorExe -ArgumentList $editorArgs -PassThru
    if (-not $p.WaitForExit($LaunchTimeoutSec * 1000)) {
        try { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep 3; if (-not $p.HasExited) { $p.Kill() } } } catch { }
    }

    if (-not $deferredReason) {
        Start-Sleep -Seconds 3
        try { if ($infoProc -and -not $infoProc.HasExited) { $infoProc.Kill() } } catch { }
        try { if ($imageProc -and -not $imageProc.HasExited) { $imageProc.Kill() } } catch { }
        $null = Invoke-Wsl -BashCommand "pkill -f 'topic echo --once /camera/camera_info' 2>/dev/null; pkill -f 'topic echo --once --full-length /camera/image/compressed' 2>/dev/null; echo stopped"
    }

    # Editor log proves the messages were BUILT (format/jpeg size/intrinsics)
    # even if the live echo is deferred.
    $builtLine = ''
    if (Test-Path $editorLog) {
        $m = Select-String -Path $editorLog -Pattern 'built CompressedImage: .*' | Select-Object -First 1
        if ($m) { $builtLine = $m.Line.Trim() }
    }

    $infoEcho = if (Test-Path $infoFile) { Get-Content $infoFile -Raw } else { '' }
    $imageEcho = if (Test-Path $imageFile) { Get-Content $imageFile -Raw } else { '' }

    # CameraInfo width/height == config (640x480).
    $infoW = $null; $infoH = $null
    if ($infoEcho -match '(?m)^\s*width:\s*(\d+)\s*$') { $infoW = [int]$Matches[1] }
    if ($infoEcho -match '(?m)^\s*height:\s*(\d+)\s*$') { $infoH = [int]$Matches[1] }
    $infoOk = ($infoW -eq 640) -and ($infoH -eq 480)

    # Decode the echoed CompressedImage base64 data to a JPEG and read its size,
    # in WSL with Python (PIL if present, else a minimal SOF marker parse). The
    # ros2 CLI prints `data:` as a Python-list-style byte array when not --no-arr;
    # extract it, write bytes, and read dimensions.
    $imageW = $null; $imageH = $null
    $decodeDetail = ''
    if (-not $deferredReason -and $imageEcho) {
        $wslImage = Convert-ToWslPath $imageFile
        $wslJpegSize = Convert-ToWslPath $jpegSize
        $decHelper = Join-Path $LogDir 'decode_jpeg.py'
        $decPy = @'
import sys, re, struct
echo_path, out_path = sys.argv[1], sys.argv[2]
txt = open(echo_path, 'r', errors='replace').read()
# ros2 echo prints CompressedImage.data as a byte array, e.g.
#   data:
#   - 255
#   - 216
#   ...
# or as "data: [255, 216, ...]". Handle both.
m = re.search(r'data:\s*\[([^\]]*)\]', txt, re.S)
if m:
    nums = [int(x) for x in re.findall(r'-?\d+', m.group(1))]
else:
    # list-item form after a `data:` line
    seg = txt.split('data:', 1)[1] if 'data:' in txt else ''
    nums = [int(x) for x in re.findall(r'(?m)^\s*-\s*(\d+)\s*$', seg)]
if not nums:
    print('NO_DATA'); open(out_path,'w').write('NO_DATA'); sys.exit(0)
b = bytes([n & 0xFF for n in nums])
# Parse JPEG SOF0/2 for dimensions without PIL.
w = h = None
i = 2
while i < len(b) - 1:
    if b[i] != 0xFF:
        i += 1; continue
    marker = b[i+1]
    if marker in (0xC0, 0xC1, 0xC2, 0xC3):
        h = struct.unpack('>H', b[i+5:i+7])[0]
        w = struct.unpack('>H', b[i+7:i+9])[0]
        break
    if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
        i += 2; continue
    seglen = struct.unpack('>H', b[i+2:i+4])[0]
    i += 2 + seglen
res = f'{w}x{h} bytes={len(b)}'
print(res)
open(out_path, 'w').write(res)
'@
        [IO.File]::WriteAllText($decHelper, ($decPy -replace "`r`n", "`n"))
        $wslDecHelper = Convert-ToWslPath $decHelper
        $dec = Invoke-Wsl -BashCommand "python3 `"$wslDecHelper`" `"$wslImage`" `"$wslJpegSize`""
        $decodeDetail = $dec.Output.Trim()
        if ($decodeDetail -match '(\d+)x(\d+)') { $imageW = [int]$Matches[1]; $imageH = [int]$Matches[2] }
    }
    $imageOk = ($imageW -eq 640) -and ($imageH -eq 480)

    return [pscustomobject]@{
        BuiltLine      = $builtLine
        InfoOk         = $infoOk
        InfoW          = $infoW
        InfoH          = $infoH
        InfoEcho       = $infoEcho
        ImageOk        = $imageOk
        ImageW         = $imageW
        ImageH         = $imageH
        DecodeDetail   = $decodeDetail
        EditorLog      = $editorLog
        DeferredReason = $deferredReason
    }
}

# --- Orchestration ---------------------------------------------------------
Clear-UbtCache

if (-not $SkipBuild) {
    $build = Invoke-Build
    Write-Step "Build: Ok=$($build.Ok) $($build.Detail)"
    if (-not $build.Ok) { Write-Error "Build failed; see $($build.Log)"; exit 1 }
}

$pub = Invoke-Publish
Write-Step "Built: $($pub.BuiltLine)"
Write-Step "CameraInfo Ok=$($pub.InfoOk) ($($pub.InfoW)x$($pub.InfoH)); Image Ok=$($pub.ImageOk) ($($pub.DecodeDetail)); Deferred=$([bool]$pub.DeferredReason)"

$commit = (& git -C $RepoRoot rev-parse HEAD 2>$null)
$ev = Join-Path $EvidenceDir 'task_14_1_camera_info_echo.txt'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('# Task 14.1 — camera CompressedImage + CameraInfo publish evidence (Requirements 4.3, 4.4)')
[void]$sb.AppendLine("# Generated: $(Get-Date -Format o)")
[void]$sb.AppendLine("# Command: pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_camera_publish.ps1")
[void]$sb.AppendLine("# Commit:  $commit")
[void]$sb.AppendLine("# Engine:  $EnginePath")
[void]$sb.AppendLine('')
[void]$sb.AppendLine('## Messages built in the editor (format / jpeg bytes / CameraInfo intrinsics):')
[void]$sb.AppendLine($pub.BuiltLine)
[void]$sb.AppendLine('')
if ($pub.DeferredReason) {
    [void]$sb.AppendLine("## Live `ros2 topic echo /camera/camera_info` + `/camera/image/compressed`: DEFERRED")
    [void]$sb.AppendLine("Reason: $($pub.DeferredReason)")
} else {
    [void]$sb.AppendLine('## Live `ros2 topic echo --once /camera/camera_info` (WSL):')
    [void]$sb.AppendLine($pub.InfoEcho.Trim())
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('## Received /camera/image/compressed decoded JPEG dimensions:')
    [void]$sb.AppendLine($pub.DecodeDetail)
}
[IO.File]::WriteAllText($ev, $sb.ToString())
Write-Step "evidence written to $ev"

if (Test-Path $pub.EditorLog) { Copy-Item $pub.EditorLog (Join-Path $EvidenceDir 'task_14_1_camera_publish_editor.log') -Force }

# Message-build is the hard gate (format=jpeg + CameraInfo intrinsics observed
# in-engine). The live echo passes when reachable; it is cleanly deferred (never
# faked) when rosbridge/WSL is unavailable.
$builtOk = ($pub.BuiltLine -match "format='jpeg'") -and ($pub.BuiltLine -match 'w=640 h=480')
if (-not $builtOk) {
    Write-Host 'RESULT: FAIL — camera messages were not built with the expected format/resolution.' -ForegroundColor Red
    exit 1
}
if ($pub.DeferredReason) {
    Write-Host "RESULT: PASS (live echo DEFERRED: $($pub.DeferredReason)) — messages built with format=jpeg, CameraInfo 640x480." -ForegroundColor Yellow
    exit 0
}
if ($pub.InfoOk -and $pub.ImageOk) {
    Write-Host 'RESULT: PASS — /camera/camera_info echoed 640x480 and /camera/image/compressed decoded to a 640x480 JPEG.' -ForegroundColor Green
    exit 0
}
Write-Host "RESULT: FAIL — rosbridge reachable but echo/decode did not confirm 640x480 (info=$($pub.InfoW)x$($pub.InfoH), image=$($pub.DecodeDetail))." -ForegroundColor Red
exit 1
