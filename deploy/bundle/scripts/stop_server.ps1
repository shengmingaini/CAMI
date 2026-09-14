# scripts/stop_server.ps1 — 停止单进程服务器（start_server.ps1 的配对脚本）
#
# 注意：$pid 是 PowerShell 只读自动变量（当前会话 PID），绝不能用作局部变量名，
# 否则赋值即抛 SessionStateUnauthorizedAccessException，停止流程整个失效。
#
# 停止路径（两级）：
#   1) 优雅：touch run\stop.signal —— 服务器主循环每 500ms 检查 --stop-file，
#      发现即置停止位并删掉该文件，四角色线程自退（保住未落盘的脏数据）。
#   2) 兜底：15 秒仍未退才 taskkill /f。
# 之所以不直接用 taskkill（不带 /f）：Windows 下它对控制台进程只发 WM_CLOSE，
# 实际不会停，等于白等 2 秒再强杀。

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

$pidFile = "$root\run\server.pid"
$stopFile = "$root\run\stop.signal"

if (Test-Path $pidFile) {
    $serverPid = (Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($serverPid) {
        $proc = Get-Process -Id $serverPid -ErrorAction SilentlyContinue
        # 必须先校验进程名：pid 文件可能是过期的，而该 PID 已被系统分配给别的进程，
        # 直接 taskkill 会误杀无关进程。
        if ($proc -and $proc.ProcessName -ne "mmorpg_server") {
            # ${serverPid} 的花括号不能省：双引号里 "$serverPid:" 会被解析成
            # 作用域/驱动器限定符，直接 ParserError，整个脚本连解析都过不去。
            Write-Host "refuse to kill pid=${serverPid}: process name is '$($proc.ProcessName)', not mmorpg_server (stale pid file)"
            $proc = $null
        }
        if ($proc) {
            Set-Content -Path $stopFile -Value "stop" -Encoding ASCII
            Write-Host "requested graceful stop pid=${serverPid} (sentinel: run\stop.signal)"
            $deadline = (Get-Date).AddSeconds(15)
            while ((Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 400
                if (-not (Get-Process -Id $serverPid -ErrorAction SilentlyContinue)) { break }
            }
            if (Get-Process -Id $serverPid -ErrorAction SilentlyContinue) {
                Write-Host "graceful stop timed out (15s), force kill pid=${serverPid}"
                & taskkill.exe /f /pid $serverPid 2>$null | Out-Null
                Start-Sleep -Milliseconds 500
                if (Get-Process -Id $serverPid -ErrorAction SilentlyContinue) {
                    Write-Host "ERROR: still running pid=${serverPid}"
                } else {
                    Write-Host "mmorpg_server force stopped pid=${serverPid}"
                }
            } else {
                Write-Host "mmorpg_server stopped gracefully pid=${serverPid}"
            }
        } else {
            Write-Host "mmorpg_server already stopped (stale pid file removed)"
        }
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
} else {
    # 兜底：没有 pid 文件时按进程名停（此时无从得知哨兵路径，只能强杀）
    $procs = Get-Process -Name mmorpg_server -ErrorAction SilentlyContinue
    if ($procs) {
        foreach ($proc in $procs) { & taskkill.exe /f /pid $proc.Id 2>$null | Out-Null }
        Write-Host "stopped $($procs.Count) mmorpg_server process(es) by name"
    } else {
        Write-Host "no pid file and no mmorpg_server process (not started?)"
    }
}

# 哨兵若还在（进程没起来 / 已退出），留着会让下一次启动被立刻停掉
if (Test-Path $stopFile) { Remove-Item $stopFile -Force -ErrorAction SilentlyContinue }
