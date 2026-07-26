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
    [Parameter(ParameterSetName = 'Install')][switch]$ShowWindow,
    # Encode full-range colour. More accurate, but a client that ignores the
    # range flag will render it too contrasty.
    [Parameter(ParameterSetName = 'Install')][switch]$FullRange,

    # Registers the SYSTEM-privileged unlock helper. Separate from -Install and
    # off by default, because it is the one genuinely privileged piece here: see
    # the warning it prints.
    [Parameter(ParameterSetName = 'InstallUnlock')][switch]$InstallUnlock,
    [Parameter(ParameterSetName = 'UninstallUnlock')][switch]$UninstallUnlock
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

# Ask the host to shut down rather than killing it: a hard kill strands the
# virtual gamepad and would leave any display changes applied.
function Stop-Host {
    if (-not (Get-Process thorstream-host -ErrorAction SilentlyContinue)) { return }
    $exe = $null
    try { $exe = Resolve-Exe -Provided $ExePath } catch { }
    if ($exe) { & $exe --stop | Out-Null }

    # Only escalate if it ignored the polite request.
    Start-Sleep -Milliseconds 500
    Get-Process thorstream-host -ErrorAction SilentlyContinue | Stop-Process -Force
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

    Write-Host "Console   : http://localhost:47812/"

    if (Get-ScheduledTask -TaskName 'thorstream-unlock' -ErrorAction SilentlyContinue) {
        Write-Host "Unlock    : installed - the host can unlock this PC when locked"
    } else {
        Write-Host "Unlock    : not installed - launching a game on a locked PC will fail"
    }

    if (Test-Path $logPath) { Write-Host "Log       : $logPath" }
}

$unlockTaskName = 'thorstream-unlock'

function Assert-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This needs an elevated PowerShell (Run as administrator)."
    }
}

if ($UninstallUnlock) {
    Assert-Elevated
    if (Get-ScheduledTask -TaskName $unlockTaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $unlockTaskName -Confirm:$false
        Write-Host "Removed '$unlockTaskName'. The host can no longer unlock this PC."
    } else {
        Write-Host "No '$unlockTaskName' task was installed."
    }
    return
}

if ($InstallUnlock) {
    Assert-Elevated
    $exe = Resolve-Exe -Provided $ExePath

    Write-Host ""
    Write-Host "  READ THIS BEFORE CONTINUING" -ForegroundColor Yellow
    Write-Host "  ---------------------------"
    Write-Host "  This registers a task running as SYSTEM that reconnects your session"
    Write-Host "  to the console, which unlocks this PC without a password. That is the"
    Write-Host "  same mechanism Sunshine and Apollo use, and no credentials are stored"
    Write-Host "  anywhere by it."
    Write-Host ""
    Write-Host "  The risk is not the unlock itself, it is what it is wired to. The"
    Write-Host "  thorstream protocol has NO authentication, so anyone who can reach"
    Write-Host "  port $Port on your network can launch a game - and therefore unlock"
    Write-Host "  this PC - and then type on it via the on-screen keyboard." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Only install this on a network you trust, and never port-forward"
    Write-Host "  47810/47811 to the internet."
    Write-Host ""

    $answer = Read-Host "  Type UNLOCK to continue"
    if ($answer -cne 'UNLOCK') { Write-Host "Cancelled; nothing was changed."; return }

    # On demand only - no trigger. The host asks for it by name when it finds the
    # session locked, and it exits immediately after.
    $action = New-ScheduledTaskAction -Execute $exe -Argument '--unlock-now' -WorkingDirectory (Split-Path $exe)
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest

    Register-ScheduledTask -TaskName $unlockTaskName -Action $action `
        -Settings $settings -Principal $principal -Force | Out-Null

    # A task created by an administrator is not runnable by a non-elevated
    # process by default, and the host deliberately runs unelevated. Grant
    # read+execute to this user specifically rather than to Users at large, so
    # the ability to unlock does not extend to every account on the machine.
    $sid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
    $sddl = "D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;$sid)"
    $service = New-Object -ComObject Schedule.Service
    $service.Connect()
    $service.GetFolder('\').GetTask($unlockTaskName).SetSecurityDescriptor($sddl, 0)

    Write-Host ""
    Write-Host "Installed '$unlockTaskName'."
    Write-Host "  Runs    : $exe --unlock-now  (as SYSTEM, on demand)"
    Write-Host "  Trigger : none - only the host asks for it"
    Write-Host "  Allowed : SYSTEM, Administrators, and $env:USERNAME"
    Write-Host ""
    Write-Host "Verify it end to end with:  $exe --unlock-test"
    Write-Host "Remove it with:             .\autostart.ps1 -UninstallUnlock"
    return
}

if ($Uninstall) {
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
        Write-Host "Removed the '$taskName' autostart task."
    } else {
        Write-Host "No '$taskName' task was installed."
    }
    Stop-Host
    return
}

if (-not $Install) { Show-Status; return }

$exe = Resolve-Exe -Provided $ExePath
New-Item -ItemType Directory -Force -Path (Split-Path $logPath) | Out-Null

# No --hidden needed: the host is a GUI-subsystem binary and never allocates a
# console, so nothing flashes on screen at logon.
$arguments = "--serve --port $Port --log `"$logPath`""
if ($FullRange) { $arguments += " --full-range" }

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

Stop-Host
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 4
Show-Status
