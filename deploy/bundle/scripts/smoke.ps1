# scripts/smoke.ps1 — 部署包自检：验证四进程 + gateway 端到端链路
# 用法：powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1
# 退出码 0 = 通过。
#
# 注：不用 Start-Process（其内部环境字典遇到大小写混排的重复 Path/PATH 键会抛
# 「已添加项」异常），改用 .NET ProcessStartInfo 显式启动，行为可控且可拿退出码。

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

New-Item -ItemType Directory -Force -Path "$root\logs" | Out-Null

function Start-Daemon {
    # Start-Daemon <exe 路径> <string[] 参数> <stdout 日志> <stderr 日志> <等待?>
    param(
        [string]$Exe, [string[]]$ArgList,
        [string]$OutLog, [string]$ErrLog,
        [switch]$Wait
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false            # 必须 false 才能重定向
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($ArgList) { $psi.Arguments = ($ArgList -join ' ') }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if ($Wait) {
        $proc.WaitForExit()
        # 异步读盘防管道阻塞（进程退出后 ReadToEnd 安全）
        $outText = $proc.StandardOutput.ReadToEnd()
        $errText = $proc.StandardError.ReadToEnd()
        [System.IO.File]::WriteAllText($OutLog, $outText)
        [System.IO.File]::WriteAllText($ErrLog, $errText)
        return $proc.ExitCode
    }
    # 后台模式：起线程异步收日志，避免缓冲写满卡死子进程
    Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -Path $Event.MessageData -Value $EventArgs.Data }
    } -MessageData $OutLog | Out-Null
    Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -Path $Event.MessageData -Value $EventArgs.Data }
    } -MessageData $ErrLog | Out-Null
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()
    return $proc
}

Write-Host "=== 1. four-process --run-for smoke ==="
foreach ($name in @("dataservice", "control", "gamenode")) {
    $code = Start-Daemon "$root\bin\$name.exe" @("--run-for", "2") `
        "$root\logs\smoke_$name.log" "$root\logs\smoke_$name.err.log" -Wait
    if ($code -ne 0) { Write-Host "FAIL $name exit=$code"; exit 1 }
    Write-Host "PASS $name (exit=0)"
}

Write-Host "=== 2. gateway end-to-end (auth + heartbeat) ==="
$gw = Start-Daemon "$root\bin\gateway.exe" @("--run-for", "15") `
    "$root\logs\smoke_gateway.log" "$root\logs\smoke_gateway.err.log"
Start-Sleep -Seconds 2

$clientCode = Start-Daemon "$root\bin\gateway_smoke_client.exe" @() `
    "$root\logs\smoke_client.log" "$root\logs\smoke_client.err.log" -Wait

$gw.WaitForExit()

$clientLog = ""
if (Test-Path "$root\logs\smoke_client.log") { $clientLog = Get-Content "$root\logs\smoke_client.log" -Raw -ErrorAction SilentlyContinue }
if ($clientCode -ne 0 -or $clientLog -notmatch "SMOKE_OK") {
    Write-Host "FAIL gateway e2e (client exit=$clientCode)"
    Write-Host $clientLog
    exit 1
}
Write-Host "PASS gateway e2e (SMOKE_OK)"

$gwLog = Get-Content "$root\logs\smoke_gateway.log" -Raw -ErrorAction SilentlyContinue
if ($gwLog -match "auth_ok=1") { Write-Host "PASS gateway auth_ok=1" } else { Write-Host "WARN auth_ok not seen in gateway log" }

Write-Host ""
Write-Host "SMOKE_ALL_OK"
exit 0
