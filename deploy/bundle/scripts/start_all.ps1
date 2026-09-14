# scripts/start_all.ps1 — 一键启动全部四进程（部署包自带）
# 用法：powershell -ExecutionPolicy Bypass -File scripts\start_all.ps1
# 前置：无（MinGW 运行时 DLL 已随包附带在 bin\，进程从 bin\ 启动自动加载）
#
# 日志写到 logs\<proc>.log；PID 写到 run\，供 stop_all.ps1 停止。
# 注：用 .NET ProcessStartInfo 而非 Start-Process —— 后者遇到父进程环境里
# 大小写混排的重复 Path/PATH 键会抛「已添加项」异常，且空参数数组校验失败。

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot   # 包根目录（scripts 的上一级）

New-Item -ItemType Directory -Force -Path "$root\logs", "$root\run" | Out-Null

function Start-Daemon {
    # Start-Daemon <进程名>：从 bin\ 启动，日志重定向到 logs\<name>.log
    param([string]$Name)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$root\bin\$Name.exe"
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    # 异步收日志（事件订阅写文件），避免管道缓冲写满卡死子进程
    $outLog = "$root\logs\$Name.log"
    $errLog = "$root\logs\$Name.err.log"
    Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -Path $Event.MessageData -Value $EventArgs.Data }
    } -MessageData $outLog | Out-Null
    Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -Path $Event.MessageData -Value $EventArgs.Data }
    } -MessageData $errLog | Out-Null
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()
    Set-Content -Path "$root\run\$Name.pid" -Value $proc.Id
    Write-Host "started $Name pid=$($proc.Id) log=logs\$Name.log"
    return $proc
}

# 启动顺序按依赖：dataservice（存储）→ control（控制面）→ gamenode（Tick）→ gateway（入口）
$null = Start-Daemon "dataservice"
Start-Sleep -Milliseconds 300
$null = Start-Daemon "control"
Start-Sleep -Milliseconds 300
$null = Start-Daemon "gamenode"
Start-Sleep -Milliseconds 300
$gw = Start-Daemon "gateway"

Start-Sleep -Milliseconds 800
if ($gw.HasExited) {
    Write-Host "ERROR: gateway exited immediately (code=$($gw.ExitCode)) - check logs\gateway.log"
    exit 1
}

Write-Host ""
Write-Host "all started. stop: powershell -File scripts\stop_all.ps1"
Write-Host "gateway listening on port from config\network.json (default 9000)"
Write-Host "smoke test: powershell -File scripts\smoke.ps1"
exit 0
