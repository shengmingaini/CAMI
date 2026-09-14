# scripts/stop_all.ps1 — 优雅停止全部进程（按启动逆序）
# daemon 对 SIGINT/SIGTERM/SIGBREAK 均做优雅退出（冲刷日志/脏队列）。
# Windows 无跨进程 SIGTERM：先 taskkill（无 /f，发 WM_CLOSE），2 秒未退才 /f 强杀。

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

# 逆序停：gateway → gamenode → control → dataservice
$procs = @("gateway", "gamenode", "control", "dataservice")

foreach ($name in $procs) {
    $pidFile = "$root\run\$name.pid"
    if (Test-Path $pidFile) {
        $pid = Get-Content $pidFile -ErrorAction SilentlyContinue
        if ($pid) {
            $p = Get-Process -Id $pid -ErrorAction SilentlyContinue
            if ($p) {
                taskkill /pid $pid 2>$null | Out-Null
                Start-Sleep -Milliseconds 2000
                if (Get-Process -Id $pid -ErrorAction SilentlyContinue) {
                    Write-Host "$name did not exit gracefully, force kill pid=$pid"
                    taskkill /f /pid $pid 2>$null | Out-Null
                } else {
                    Write-Host "$name stopped pid=$pid"
                }
            } else {
                Write-Host "$name already stopped"
            }
        }
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "all stopped."
