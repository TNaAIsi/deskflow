# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

# Destructive regression fixture for disposable GitHub Actions Windows runners.
# This installs an old release, replaces test settings, upgrades, and repairs.
# The candidate remains installed and running for the package verifier to inspect.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BaselineMsi,
    [Parameter(Mandatory = $true)][string]$CandidateMsi,
    [Parameter(Mandatory = $true)][string]$Output,
    [ValidateRange(30, 600)][int]$InstallTimeoutSeconds = 180,
    [ValidateRange(5, 120)][int]$ServiceTimeoutSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Windows upgrade verification requires PowerShell 7 on Windows.' }
if ($env:GITHUB_ACTIONS -cne 'true') {
    throw 'This fixture changes machine settings and may run only in a disposable GitHub Actions job.'
}
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The disposable Windows runner must be elevated to test MSI installation and its service.'
}

$outputPath = [IO.Path]::GetFullPath($Output)
$outputDirectory = [IO.Path]::GetDirectoryName($outputPath)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$baselinePath = (Resolve-Path -LiteralPath $BaselineMsi).Path
$candidatePath = (Resolve-Path -LiteralPath $CandidateMsi).Path
$installedRoot = Join-Path $env:ProgramFiles 'Deskflow'
$settingsPath = Join-Path $env:ProgramData 'Deskflow/Deskflow.conf'
$systemUserSettings = Join-Path $env:SystemRoot 'System32/config/systemprofile/AppData/Roaming/Deskflow/Deskflow.conf'
$installer = New-Object -ComObject WindowsInstaller.Installer
$report = [ordered]@{
    passed = $false
    github_actions = $env:GITHUB_ACTIONS
    baseline = $null
    candidate = $null
    installations = @()
    service_checks = @()
    settings_path = $settingsPath
    settings_backups = @()
    settings_checks = @()
}

function Get-MsiProperty([string]$Path, [string]$Property) {
    $database = $null
    $view = $null
    $record = $null
    try {
        $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Path, 0))
        $query = "SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = '$Property'"
        $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($query))
        $null = $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if (-not $record) { throw "MSI property $Property is missing in $Path" }
        return [string]$record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @(1))
    } finally {
        foreach ($item in @($record, $view, $database)) {
            if ($null -ne $item) { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($item) | Out-Null }
        }
    }
}

function Get-PackageRecord([string]$Path) {
    return [ordered]@{
        path = $Path
        product_code = Get-MsiProperty $Path 'ProductCode'
        upgrade_code = Get-MsiProperty $Path 'UpgradeCode'
        version = Get-MsiProperty $Path 'ProductVersion'
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Invoke-Msi([string]$Step, [string]$Mode, [string]$Path) {
    $logPath = Join-Path $outputDirectory ("windows-upgrade-$Step-install.log")
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $env:SystemRoot 'System32/msiexec.exe'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    foreach ($argument in @($Mode, $Path, '/qn', '/norestart', '/L*v', $logPath)) {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $started = $false
    $entry = [ordered]@{ step = $Step; mode = $Mode; log = $logPath; exit_code = $null; timed_out = $false }
    $report.installations += $entry
    try {
        if (-not $process.Start()) { throw "Failed to launch Windows Installer for $Step" }
        $started = $true
        if (-not $process.WaitForExit($InstallTimeoutSeconds * 1000)) {
            $entry.timed_out = $true
            $process.Kill($true)
            $process.WaitForExit(5000) | Out-Null
            throw "MSI $Step exceeded $InstallTimeoutSeconds seconds; a hidden startup dialog or hang is a failure."
        }
        $entry.exit_code = $process.ExitCode
        if ($process.ExitCode -notin @(0, 3010)) {
            if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath -Tail 100 | Write-Host }
            throw "MSI $Step failed with exit code $($process.ExitCode). See $logPath"
        }
    } finally {
        if ($started -and -not $process.HasExited) {
            $process.Kill($true)
            $process.WaitForExit(5000) | Out-Null
        }
        $process.Dispose()
    }
}

function Wait-DeskflowService([string]$Step, [ServiceProcess.ServiceControllerStatus]$Status) {
    $controller = Get-Service -Name 'Deskflow'
    try {
        $controller.WaitForStatus($Status, [TimeSpan]::FromSeconds($ServiceTimeoutSeconds))
        $service = Get-CimInstance -ClassName Win32_Service -Filter "Name='Deskflow'"
        $entry = [ordered]@{
            step = $Step; state = $service.State; process_id = $service.ProcessId
            executable = $service.PathName; account = $service.StartName
            win32_exit_code = $service.ExitCode; service_exit_code = $service.ServiceSpecificExitCode
        }
        $report.service_checks += $entry
        if ($Status -eq [ServiceProcess.ServiceControllerStatus]::Running) {
            if ($service.ProcessId -eq 0) { throw "No daemon process after $Step" }
            $process = Get-Process -Id $service.ProcessId
            try {
                if ($process.Path -ine (Join-Path $installedRoot 'deskflow-daemon.exe') -or $process.SessionId -ne 0) {
                    throw "Service after $Step is not the installed daemon in session 0."
                }
            } finally { $process.Dispose() }
        }
        return $entry
    } finally { $controller.Dispose() }
}

function Stop-DeskflowService([string]$Step) {
    $controller = Get-Service -Name 'Deskflow'
    try { $controller.Stop() } finally { $controller.Dispose() }
    $null = Wait-DeskflowService $Step ([ServiceProcess.ServiceControllerStatus]::Stopped)
}

function Assert-StableService([string]$Step) {
    $first = Wait-DeskflowService $Step ([ServiceProcess.ServiceControllerStatus]::Running)
    Start-Sleep -Seconds 2
    $second = Wait-DeskflowService "$Step stable" ([ServiceProcess.ServiceControllerStatus]::Running)
    if ($first.process_id -ne $second.process_id) { throw "Service crashed and respawned after $Step" }
}

function Backup-TestSettings([string]$Path, [string]$Name, [bool]$Remove) {
    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    $backup = Join-Path $outputDirectory $Name
    if ($exists) {
        Copy-Item -LiteralPath $Path -Destination $backup
        if ($Remove) { Remove-Item -LiteralPath $Path -Force }
    }
    $report.settings_backups += [ordered]@{ path = $Path; existed = $exists; backup = $(if ($exists) { $backup } else { $null }) }
}

function Read-QtIni([string]$Path) {
    $result = @{}
    $group = ''
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.*)\]$') { $group = $Matches[1]; continue }
        if (-not $trimmed -or $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) { continue }
        $equals = $line.IndexOf('=')
        if ($equals -lt 0) { continue }
        $key = $line.Substring(0, $equals).Trim().Replace('\', '/')
        $result["$group/$key"] = $line.Substring($equals + 1).Trim()
    }
    return $result
}

function Assert-MigratedSettings([string]$Step) {
    # RUNNING is reported before all startup work is complete, so wait for the
    # durable QSettings flush rather than assuming the file changed immediately.
    $deadline = [DateTime]::UtcNow.AddSeconds($ServiceTimeoutSeconds)
    do {
        $settings = Read-QtIni $settingsPath
        if ($settings.ContainsKey('computer_Mac/modifierShift')) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    $expected = [ordered]@{
        'core/computerName' = 'UpgradeLocal'
        'server/preventFullscreenSwitch' = 'true'
        'server/switchDelay' = '137'
        'computer_Windows/name' = 'Windows'
        'computer_Windows/halfDuplexCapsLock' = 'true'
        'computer_Windows/halfDuplexNumLock' = 'false'
        'computer_Windows/halfDuplexScrollLock' = 'false'
        'computer_Windows/xtestIsXineramaUnaware' = 'false'
        'computer_Windows/modifierShift' = 'meta'
        'computer_Windows/modifierCtrl' = 'super'
        'computer_Windows/modifierAlt' = 'alt'
        'computer_Windows/modifierMeta' = 'meta'
        'computer_Windows/modifierSuper' = 'super'
        'computer_Windows/modifierAltGr' = 'altgr'
        'computer_Mac/name' = 'Mac'
        'computer_Mac/halfDuplexCapsLock' = 'false'
        'computer_Mac/modifierShift' = 'shift'
        'computer_Mac/modifierCtrl' = 'ctrl'
        'computer_Mac/modifierAlt' = 'alt'
        'computer_Mac/modifierMeta' = 'meta'
        'computer_Mac/modifierSuper' = 'super'
        'computer_Mac/modifierAltGr' = 'altgr'
        'computer_LegacyDevice/name' = 'LegacyDevice'
    }
    foreach ($key in $expected.Keys) {
        if (-not $settings.ContainsKey($key) -or $settings[$key] -cne $expected[$key]) {
            throw "Settings after ${Step}: $key expected '$($expected[$key])', got '$($settings[$key])'."
        }
    }
    if ($settings.ContainsKey('screen_LegacyDevice/name')) { throw 'Legacy screen_ group was not migrated.' }
    $snapshot = Join-Path $outputDirectory ("windows-upgrade-settings-$Step.conf")
    Copy-Item -LiteralPath $settingsPath -Destination $snapshot
    $report.settings_checks += [ordered]@{ step = $Step; passed = $true; snapshot = $snapshot; values = $expected }
}

function Assert-CandidateInstalled {
    $oldKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$($report.baseline.product_code)"
    $newKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$($report.candidate.product_code)"
    if (Test-Path -LiteralPath $oldKey) { throw 'Major upgrade left the baseline MSI product registered.' }
    if (-not (Test-Path -LiteralPath $newKey)) { throw 'Candidate MSI product is not registered.' }
    $installed = Get-ItemProperty -LiteralPath $newKey
    if ([version]$installed.DisplayVersion -ne [version]$report.candidate.version) {
        throw "Unexpected registered product version: $($installed.DisplayVersion)"
    }
    foreach ($name in @('deskflow.exe', 'deskflow-core.exe', 'deskflow-daemon.exe')) {
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $installedRoot $name)).ProductVersion
        if ([version]$version -ne [version]$report.candidate.version) { throw "$name was not upgraded: $version" }
    }
}

try {
    $report.baseline = Get-PackageRecord $baselinePath
    $report.candidate = Get-PackageRecord $candidatePath
    if ($report.baseline.version -notmatch '^1\.28\.0([.]|$)') { throw 'Baseline must be the released 1.28.0 MSI.' }
    if ($report.baseline.product_code -eq $report.candidate.product_code -or
        $report.baseline.upgrade_code -ne $report.candidate.upgrade_code -or
        [version]$report.candidate.version -le [version]$report.baseline.version) {
        throw 'Expected a newer candidate with the same UpgradeCode and a distinct ProductCode.'
    }
    if (Get-Service -Name 'Deskflow' -ErrorAction SilentlyContinue) {
        throw 'Upgrade fixture requires a clean runner with no existing Deskflow service.'
    }
    Invoke-Msi 'baseline' '/i' $baselinePath
    Assert-StableService 'baseline installation'
    Stop-DeskflowService 'before legacy fixture'
    Backup-TestSettings $settingsPath 'windows-upgrade-settings-original.conf' $false
    # Baseline startup may create a SYSTEM-user file, which takes precedence over
    # ProgramData. Remove only that backed-up CI fixture so the intended legacy
    # machine configuration is actually loaded by the candidate service.
    Backup-TestSettings $systemUserSettings 'windows-upgrade-systemprofile-original.conf' $true
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($settingsPath)) | Out-Null
    @'
[core]
computerName=UpgradeLocal

[server]
preventFullscreenSwitch=true
switchDelay=137

[internalConfig]
switchDelay=42
screens\size=2
screens\1\name=Windows
screens\1\fixArray\size=1
screens\1\fixArray\1\fix=false
screens\1\modifierArray\size=1
screens\1\modifierArray\1\modifier=3
screens\2\name=Mac

[computer_Windows]
halfDuplexCapsLock=true
modifierCtrl=super

[screen_LegacyDevice]
name=LegacyDevice
'@ | Set-Content -LiteralPath $settingsPath -Encoding utf8
    Copy-Item -LiteralPath $settingsPath -Destination (Join-Path $outputDirectory 'windows-upgrade-settings-seeded.conf')
    Invoke-Msi 'candidate' '/i' $candidatePath
    Assert-StableService 'major upgrade'
    Assert-CandidateInstalled
    Assert-MigratedSettings 'upgraded'
    Stop-DeskflowService 'before repair'
    Invoke-Msi 'repair' '/fa' $candidatePath
    Assert-StableService 'MSI repair'
    Assert-CandidateInstalled
    Assert-MigratedSettings 'repaired'
    $report.passed = $true
    Write-Host 'Baseline install, legacy-settings major upgrade, and MSI repair passed; candidate remains installed.'
} catch {
    $report['error'] = $_.Exception.ToString()
    throw
} finally {
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $outputPath -Encoding utf8
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null
}
