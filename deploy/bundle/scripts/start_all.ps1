# scripts/start_all.ps1 — 一键启动全部四进程（多机扩展部署形态）
# 用法：powershell -ExecutionPolicy Bypass -File scripts\start_all.ps1
# 前置：无（MinGW 运行时 DLL 已随包附带在 bin\，进程从 bin\ 启动自动加载）
#
# 日常开发/小规模部署请用 scripts\start_server.ps1（单进程跑全部角色）。
#
# 启动方式：WMI Win32_Process.Create（完全脱离宿主，不继承 shell 的 stdout 句柄，
# 避免调用方管道不关导致命令假死）。日志由各进程自己写 --log-file。
# PID 写到 run\<name>.pid，供 stop_all.ps1 停止。

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot   # 包根目录（scripts 的上一级）

New-Item -ItemType Directory -Force -Path "$root\logs", "$root\run" | Out-Null

function Start-Daemon {
    param([string]$Name)
    $exe = "$root\bin\$Name.exe"
    $log = "$root\logs\$Name.log"
    $stopFile = "$root\run\$Name.stop.signal"
    if (Test-Path $stopFile) { Remove-Item $stopFile -Force -ErrorAction SilentlyContinue }
    # --stop-file：Windows 无跨进程信号，stop_all.ps1 靠 touch 各自哨兵请求优雅退出
    $cmdLine = "`"$exe`" --config `"$root\config`" --log-file `"$log`" --no-console --stop-file `"$stopFile`""
    $res = Invoke-CimMethod -ClassName Win32_Process -MethodName Create `
        -Arguments @{ CommandLine = $cmdLine; CurrentDirectory = $root }
    if ($res.ReturnValue -ne 0) {
        Write-Host "ERROR: failed to start $Name (WMI ReturnValue=$($res.ReturnValue))"
        exit 1
    }
    Set-Content -Path "$root\run\$Name.pid" -Value $res.ProcessId
    Write-Host "started $Name pid=$($res.ProcessId) log=logs\$Name.log"
    return $res.ProcessId
}

# 启动顺序按依赖：dataservice（存储）→ control（控制面）→ gamenode（Tick）→ gateway（入口）
Start-Daemon "dataservice" | Out-Null
Start-Sleep -Milliseconds 300
Start-Daemon "control" | Out-Null
Start-Sleep -Milliseconds 300
Start-Daemon "gamenode" | Out-Null
Start-Sleep -Milliseconds 300
$gwPid = Start-Daemon "gateway"

Start-Sleep -Milliseconds 800
$gw = Get-Process -Id $gwPid -ErrorAction SilentlyContinue
if (-not $gw) {
    Write-Host "ERROR: gateway (pid=$gwPid) exited immediately - check logs\gateway.log"
    exit 1
}

Write-Host ""
Write-Host "all started. stop: powershell -File scripts\stop_all.ps1"
Write-Host "gateway listening on port from config\network.json (default 9000)"
Write-Host "smoke test: powershell -File scripts\smoke.ps1"
exit 0
