# scripts/start_server.ps1 — 单进程一键启动（推荐：开发/测试/小规模部署）
# 用法：powershell -ExecutionPolicy Bypass -File scripts\start_server.ps1
#
# 与 start_all.ps1 的区别：本脚本只启动一个 mmorpg_server.exe，
# 进程内含全部四个角色（dataservice/control/gamenode/gateway 各占一线程），
# Ctrl+C 或 stop_server.ps1 一键优雅全停。多机扩展部署用 start_all.ps1。

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

New-Item -ItemType Directory -Force -Path "$root\logs", "$root\run" | Out-Null

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$root\bin\mmorpg_server.exe"
$psi.WorkingDirectory = $root
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$proc = [System.Diagnostics.Process]::Start($psi)

# 异步收日志（事件订阅写文件），避免管道缓冲写满卡死子进程
Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) { Add-Content -Path $Event.MessageData -Value $EventArgs.Data }
} -MessageData "$root\logs\server.log" | Out-Null
Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
    if ($EventArgs.Data) { Add-Content -Path $Event.MessageData -Value $EventArgs.Data }
} -MessageData "$root\logs\server.err.log" | Out-Null
$proc.BeginOutputReadLine()
$proc.BeginErrorReadLine()

Set-Content -Path "$root\run\server.pid" -Value $proc.Id
Start-Sleep -Milliseconds 800

if ($proc.HasExited) {
    Write-Host "ERROR: mmorpg_server exited immediately (code=$($proc.ExitCode)) - check logs\server.log"
    exit 1
}

Write-Host "started mmorpg_server pid=$($proc.Id)"
Write-Host "roles: dataservice + control + gamenode + gateway (single process)"
Write-Host "gateway listening on port from config\network.json (default 9000)"
Write-Host "logs: logs\server.log"
Write-Host "stop: powershell -File scripts\stop_server.ps1"
exit 0
