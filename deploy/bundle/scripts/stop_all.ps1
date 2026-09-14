# scripts/stop_all.ps1 — 停止全部四进程（按启动逆序）
#
# 注意：$pid 是 PowerShell 只读自动变量，不能用作局部变量名（见 stop_server.ps1 注释）。
#
# 与 stop_server.ps1 一样走两级：先 touch 各自的 run\<name>.stop.signal 请求优雅退出，
# 10 秒未退再 taskkill /f。Windows 无跨进程信号，taskkill（不带 /f）对控制台进程无效。

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

# 逆序停：gateway → gamenode → control → dataservice
$names = @("gateway", "gamenode", "control", "dataservice")

foreach ($name in $names) {
    $pidFile = "$root\run\$name.pid"
    $stopFile = "$root\run\$name.stop.signal"
    if (Test-Path $pidFile) {
        $procId = (Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($procId) {
            $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
            # 校验进程名，避免过期 pid 文件导致误杀复用同一 PID 的无关进程
            if ($proc -and $proc.ProcessName -ne $name) {
                # ${name} 必须带花括号：双引号里 "$name:" 会被解析成作用域/驱动器限定符而报 ParserError
                Write-Host "${name}: refuse to kill pid=${procId} (actual process '$($proc.ProcessName)', stale pid file)"
                $proc = $null
            }
            if ($proc) {
                Set-Content -Path $stopFile -Value "stop" -Encoding ASCII
                Write-Host "${name}: requested graceful stop pid=${procId}"
                $deadline = (Get-Date).AddSeconds(10)
                while ((Get-Date) -lt $deadline) {
                    Start-Sleep -Milliseconds 400
                    if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { break }
                }
                if (Get-Process -Id $procId -ErrorAction SilentlyContinue) {
                    Write-Host "${name}: graceful stop timed out (10s), force kill pid=${procId}"
                    & taskkill.exe /f /pid $procId 2>$null | Out-Null
                } else {
                    Write-Host "${name}: stopped gracefully pid=${procId}"
                }
            } else {
                Write-Host "${name}: already stopped"
            }
        }
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
        if (Test-Path $stopFile) { Remove-Item $stopFile -Force -ErrorAction SilentlyContinue }
    } else {
        $byName = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($byName) {
            foreach ($proc in $byName) { & taskkill.exe /f /pid $proc.Id 2>$null | Out-Null }
            Write-Host "${name}: stopped by process name"
        }
    }
}
Write-Host "all stopped."
