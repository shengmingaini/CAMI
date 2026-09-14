# scripts/stop_server.ps1 — 停止单进程服务器（start_server.ps1 的配对脚本）
# daemon 对 SIGINT/SIGTERM/SIGBREAK 均做优雅退出（冲刷日志与脏队列）。
# Windows 无跨进程 SIGTERM：先 taskkill（无 /f），2 秒未退才 /f 强杀。

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

$pidFile = "$root\run\server.pid"
if (Test-Path $pidFile) {
    $pid = Get-Content $pidFile -ErrorAction SilentlyContinue
    if ($pid) {
        $p = Get-Process -Id $pid -ErrorAction SilentlyContinue
        if ($p) {
            taskkill /pid $pid 2>$null | Out-Null
            Start-Sleep -Milliseconds 2000
            if (Get-Process -Id $pid -ErrorAction SilentlyContinue) {
                Write-Host "mmorpg_server did not exit gracefully, force kill pid=$pid"
                taskkill /f /pid $pid 2>$null | Out-Null
            } else {
                Write-Host "mmorpg_server stopped gracefully pid=$pid"
            }
        } else {
            Write-Host "mmorpg_server already stopped"
        }
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
} else {
    Write-Host "no pid file (server not started by start_server.ps1?)"
}
