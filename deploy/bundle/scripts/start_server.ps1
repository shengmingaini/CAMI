# scripts/start_server.ps1 — 单进程一键启动（推荐：开发/测试/小规模部署）
# 用法：powershell -ExecutionPolicy Bypass -File scripts\start_server.ps1
#
# 说明：只启动一个 mmorpg_server.exe，进程内含全部四个角色
# （dataservice / control / gamenode / gateway 各占一线程），
# stop_server.ps1 或 Ctrl+C 可停止。多机扩展部署改用 start_all.ps1。
#
# 启动方式说明（重要）：
#   用 WMI Win32_Process.Create 而非 .NET Process.Start —— 后者会让被拉起的
#   服务器继承宿主 shell 的 stdout 句柄，导致「启动脚本已退出但调用方管道不关」
#   的假死（CI/自动化里表现为命令永不返回）。WMI 创建的是完全脱离的进程。
#   日志因此改由服务器进程自己写文件（--log-file），不再依赖宿主 stdout。

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

New-Item -ItemType Directory -Force -Path "$root\logs", "$root\run" | Out-Null

$exe = "$root\bin\mmorpg_server.exe"
$log = "$root\logs\server.log"
$stopFile = "$root\run\stop.signal"
# 启动前先清掉上一次可能残留的哨兵，否则服务器一起来就会被它立刻停掉
if (Test-Path $stopFile) { Remove-Item $stopFile -Force -ErrorAction SilentlyContinue }
# --stop-file：Windows 无跨进程信号，stop_server.ps1 靠「touch 该文件」请求优雅退出
$cmdLine = "`"$exe`" --config `"$root\config`" --log-file `"$log`" --no-console --stop-file `"$stopFile`""

$res = Invoke-CimMethod -ClassName Win32_Process -MethodName Create `
    -Arguments @{ CommandLine = $cmdLine; CurrentDirectory = $root }

if ($res.ReturnValue -ne 0) {
    Write-Host "ERROR: failed to start mmorpg_server (WMI ReturnValue=$($res.ReturnValue))"
    exit 1
}

$procId = $res.ProcessId
Set-Content -Path "$root\run\server.pid" -Value $procId

# 给它一点时间绑定端口；启动失败（如端口占用）会立刻退出
Start-Sleep -Milliseconds 900
$proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
if (-not $proc) {
    Write-Host "ERROR: mmorpg_server (pid=$procId) exited immediately - check $log"
    exit 1
}

Write-Host "started mmorpg_server pid=$procId"
Write-Host "roles:   dataservice + control + gamenode + gateway (single process)"
Write-Host "gateway: listening on port from config\network.json (default 9000)"
Write-Host "log:     logs\server.log"
Write-Host "stop:    powershell -File scripts\stop_server.ps1  (graceful via run\stop.signal)"
exit 0
