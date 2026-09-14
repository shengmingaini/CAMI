# scripts/stop_all.ps1 — 停止全部四进程（按启动逆序）
# 注意：$pid 是 PowerShell 只读自动变量，不能用作局部变量名（见 stop_server.ps1 注释）。
# Windows 无跨进程 SIGTERM：先 taskkill（不带 /f），2 秒未退再 /f 强杀。

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

# 逆序停：gateway → gamenode → control → dataservice
$names = @("gateway", "gamenode", "control", "dataservice")

foreach ($name in $names) {
    $pidFile = "$root\run\$name.pid"
    if (Test-Path $pidFile) {
        $procId = (Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($procId) {
            $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
            if ($proc) {
                & taskkill.exe /pid $procId 2>$null | Out-Null
                Start-Sleep -Milliseconds 2000
                if (Get-Process -Id $procId -ErrorAction SilentlyContinue) {
                    Write-Host "$name did not stop on request, force kill pid=$procId"
                    & taskkill.exe /f /pid $procId 2>$null | Out-Null
                } else {
                    Write-Host "$name stopped pid=$procId"
                }
            } else {
                Write-Host "$name already stopped"
            }
        }
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
    } else {
        $byName = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($byName) {
            foreach ($proc in $byName) { & taskkill.exe /f /pid $proc.Id 2>$null | Out-Null }
            Write-Host "$name stopped by process name"
        }
    }
}
Write-Host "all stopped."
