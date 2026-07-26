<#
.SYNOPSIS
    Installs, removes or inspects the thorstream host autostart task.

.DESCRIPTION
    Registers a Scheduled Task that starts the host when you log in and restarts
    it if it stops unexpectedly.

    Deliberately NOT a Windows service: services run in session 0, which is
    isolated from your desktop, and Windows.Graphics.Capture can only see windows
    in the interactive session. A service would start happily and capture nothing.

.EXAMPLE
    .\autostart.ps1 -Install
    .\autostart.ps1 -Status
    .\autostart.ps1 -Uninstall
#>
[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Install')][switch]$Install,
    [Parameter(ParameterSetName = 'Uninstall')][switch]$Uninstall,
    [Parameter(ParameterSetName = 'Status')][switch]$Status,

    [Parameter(ParameterSetName = 'Install')][string]$ExePath,
    [Parameter(ParameterSetName = 'Install')][int]$Port = 47810,
    # Show the console window instead of running detached.
    [Parameter(ParameterSetName = 'Install')][switch]$ShowWindow
)

$ErrorActionPreference = 'Stop'
$taskName = 'thorstream-host'
$logPath  = Join-Path $env:LOCALAPPDATA 'thorstream\host.log'

function Resolve-Exe {
    param([string]$Provided)
    if ($Provided) {
        if (-not (Test-Path $Provided)) { throw "No executable at $Provided" }
        return (Resolve-Path $Provided).Path
    }
    $candidate = Join-Path $PSScriptRoot 'build\thorstream-host.exe'
    if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    throw "Could not find thorstream-host.exe. Build it first, or pass -ExePath."
}

function Show-Status {
    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if (-not $task) {
        Write-Host "Autostart : not installed"
    } else {
        $info = Get-ScheduledTaskInfo -TaskName $taskName
        Write-Host "Autostart : installed ($($task.State))"
        Write-Host "Last run  : $($info.LastRunTime)  result=$($info.LastTaskResult)"
        Write-Host "Next run  : $($info.NextRunTime)"
    }

    $process = Get-Process thorstream-host -ErrorAction SilentlyContinue
    if ($process) {
        Write-Host "Process   : running (PID $($process.Id -join ', '))"
    } else {
        Write-Host "Process   : not running"
    }

    # The only check that really matters: is the port actually accepting?
    $listening = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
    if ($listening) {
        Write-Host "Listening : yes, on port $Port"
        Get-NetIPAddress -AddressFamily IPv4 |
            Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
            ForEach-Object { Write-Host "            $($_.IPAddress):$Port  ($($_.InterfaceAlias))" }
    } else {
        Write-Host "Listening : NO - the client will not be able to connect"
    }

    if (Test-Path $logPath) { Write-Host "Log       : $logPath" }
}

if ($Uninstall) {
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
        Write-Host "Removed the '$taskName' autostart task."
    } else {
        Write-Host "No '$taskName' task was installed."
    }
    Get-Process thorstream-host -ErrorAction SilentlyContinue | Stop-Process -Force
    return
}

if (-not $Install) { Show-Status; return }

$exe = Resolve-Exe -Provided $ExePath
New-Item -ItemType Directory -Force -Path (Split-Path $logPath) | Out-Null

$arguments = "--serve --port $Port --log `"$logPath`""
if (-not $ShowWindow) { $arguments += " --hidden" }

$action = New-ScheduledTaskAction -Execute $exe -Argument $arguments -WorkingDirectory (Split-Path $exe)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME

# ExecutionTimeLimit 0 = never kill it for running too long, which is the entire
# point. RestartCount covers a crash; StartWhenAvailable covers a missed logon.
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -DontStopOnIdleEnd `
    -StartWhenAvailable `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit (New-TimeSpan -Seconds 0)

$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force | Out-Null

Write-Host "Installed '$taskName'."
Write-Host "  Runs    : $exe $arguments"
Write-Host "  Trigger : at logon for $env:USERNAME"
Write-Host ""

Get-Process thorstream-host -ErrorAction SilentlyContinue | Stop-Process -Force
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 4
Show-Status
