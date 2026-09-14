# scripts/stop_server.ps1 — 停止单进程服务器（start_server.ps1 的配对脚本）
#
# 注意：$pid 是 PowerShell 只读自动变量（当前会话 PID），绝不能用作局部变量名，
# 否则赋值即抛 SessionStateUnauthorizedAccessException，停止流程整个失效。
#
# Windows 无法从外部给控制台进程发 SIGTERM/Ctrl+C：先 taskkill（不带 /f），
# 2 秒未退再 /f 强杀。需要「优雅退出」请手动运行并在窗口按 Ctrl+C。

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

$pidFile = "$root\run\server.pid"
if (Test-Path $pidFile) {
    $serverPid = (Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($serverPid) {
        $proc = Get-Process -Id $serverPid -ErrorAction SilentlyContinue
        if ($proc) {
            & taskkill.exe /pid $serverPid 2>$null | Out-Null
            Start-Sleep -Milliseconds 2000
            if (Get-Process -Id $serverPid -ErrorAction SilentlyContinue) {
                Write-Host "mmorpg_server did not stop on request, force kill pid=$serverPid"
                & taskkill.exe /f /pid $serverPid 2>$null | Out-Null
                Start-Sleep -Milliseconds 500
                if (Get-Process -Id $serverPid -ErrorAction SilentlyContinue) {
                    Write-Host "ERROR: still running pid=$serverPid"
                } else {
                    Write-Host "mmorpg_server force stopped pid=$serverPid"
                }
            } else {
                Write-Host "mmorpg_server stopped pid=$serverPid"
            }
        } else {
            Write-Host "mmorpg_server already stopped (stale pid file removed)"
        }
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
} else {
    # 兜底：没有 pid 文件时按进程名停
    $procs = Get-Process -Name mmorpg_server -ErrorAction SilentlyContinue
    if ($procs) {
        foreach ($proc in $procs) { & taskkill.exe /f /pid $proc.Id 2>$null | Out-Null }
        Write-Host "stopped $($procs.Count) mmorpg_server process(es) by name"
    } else {
        Write-Host "no pid file and no mmorpg_server process (not started?)"
    }
}
