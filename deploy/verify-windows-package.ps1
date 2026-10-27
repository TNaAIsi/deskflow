# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

# Run only on a disposable Windows packaging runner. Service checks deliberately
# stop and restart the installed Deskflow service; they do not change its config.
[CmdletBinding()]
param(
    [string]$InstalledDirectory = (Join-Path $env:ProgramFiles 'Deskflow'),
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [string]$PortableZip,
    [string]$ServiceName = 'Deskflow',
    [switch]$SkipServiceChecks,
    [ValidateRange(5, 120)][int]$TimeoutSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Windows package verification requires PowerShell 7 on Windows.' }

$installedRoot = [IO.Path]::GetFullPath($InstalledDirectory).TrimEnd('\', '/')
$outputPath = [IO.Path]::GetFullPath($Output)
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('deskflow-package-check-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$report = [ordered]@{
    passed = $false
    expected_version = $ExpectedVersion
    installed_directory = $installedRoot
    clean_path = @($env:SystemRoot, (Join-Path $env:SystemRoot 'System32'),
        (Join-Path $env:SystemRoot 'System32/Wbem')) -join ';'
    executables = @()
    service_checks = @()
    service_modules = @()
    package_libraries = @()
    portable_executables = @()
}

function Get-BinaryRecord([string]$Path) {
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    return [ordered]@{
        path = $Path
        file_version = $version.FileVersion
        product_version = $version.ProductVersion
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Invoke-CleanVersion([string]$Directory, [string]$Name, [bool]$RequireVersion) {
    $executable = Join-Path $Directory $Name
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Packaged executable is missing: $executable"
    }
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $executable
    $info.ArgumentList.Add('--version')
    # The working directory must not accidentally supply a missing build DLL.
    $info.WorkingDirectory = $scratch
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Environment['PATH'] = $report.clean_path
    foreach ($key in @($info.Environment.Keys)) {
        if ($key -match '^(QT_|QML|QTDIR$|VCPKG|VSINSTALLDIR$|VCINSTALLDIR$|VCToolsInstallDir$)') {
            $info.Environment.Remove($key) | Out-Null
        }
    }
    $info.Environment['APPDATA'] = Join-Path $scratch 'roaming'
    $info.Environment['LOCALAPPDATA'] = Join-Path $scratch 'local'
    $info.Environment['XDG_CONFIG_HOME'] = Join-Path $scratch 'config'
    $info.Environment['XDG_STATE_HOME'] = Join-Path $scratch 'state'

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $started = $false
    try {
        if (-not $process.Start()) { throw "Failed to create process: $executable" }
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "$Name did not exit within $TimeoutSeconds seconds. A hidden dependency dialog or startup hang is a failure."
        }
        $text = ($stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()).Trim()
        if ($process.ExitCode -ne 0) {
            throw "$Name failed with exit code $($process.ExitCode) under a clean PATH. Output: $text"
        }
        $binary = Get-BinaryRecord $executable
        if ($RequireVersion -and $text -notmatch [regex]::Escape($ExpectedVersion)) {
            throw "$Name returned an unexpected version: $text"
        }
        if ($binary.product_version -notmatch ('^' + [regex]::Escape($ExpectedVersion) + '([.\s-]|$)')) {
            throw "$Name has an unexpected PE product version: $($binary.product_version)"
        }
        $binary['launched'] = $true
        $binary['exit_code'] = $process.ExitCode
        $binary['version_output'] = $text
        return $binary
    } finally {
        if ($started -and -not $process.HasExited) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        $process.Dispose()
    }
}

function Get-DeskflowService {
    $escaped = $ServiceName.Replace("'", "''")
    $service = Get-CimInstance -ClassName Win32_Service -Filter "Name='$escaped'"
    if (-not $service) { throw "The installed service $ServiceName is missing." }
    return $service
}

function Assert-RunningService([string]$Step) {
    $controller = Get-Service -Name $ServiceName
    $controller.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,
        [TimeSpan]::FromSeconds($TimeoutSeconds))
    $service = Get-DeskflowService
    $expectedDaemon = Join-Path $installedRoot 'deskflow-daemon.exe'
    if ($service.State -ne 'Running' -or $service.ProcessId -eq 0) {
        throw "Service is not running after ${Step}: $($service.State)"
    }
    if ($service.StartName -notin @('LocalSystem', 'NT AUTHORITY\SYSTEM')) {
        throw "Service account is incorrect: $($service.StartName)"
    }
    if ($service.PathName.Trim() -ine ('"' + $expectedDaemon + '"')) {
        throw "Service executable must be the quoted installed daemon path: $($service.PathName)"
    }
    $process = Get-Process -Id $service.ProcessId
    try {
        if ($process.Path -ine $expectedDaemon -or $process.SessionId -ne 0) {
            throw "SCM did not launch the installed daemon in session 0: $($process.Path), session $($process.SessionId)"
        }
    } finally { $process.Dispose() }
    $report.service_checks += [ordered]@{
        step = $Step
        state = $service.State
        process_id = $service.ProcessId
        executable = $service.PathName
        account = $service.StartName
        start_mode = $service.StartMode
        win32_exit_code = $service.ExitCode
        service_exit_code = $service.ServiceSpecificExitCode
    }
    return $service.ProcessId
}

try {
    foreach ($name in @('deskflow.exe', 'deskflow-core.exe', 'deskflow-daemon.exe')) {
        $report.executables += Invoke-CleanVersion $installedRoot $name $true
    }
    $report.package_libraries = @(Get-ChildItem -LiteralPath $installedRoot -Filter '*.dll' |
        Sort-Object Name | ForEach-Object { Get-BinaryRecord $_.FullName })
    foreach ($name in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
        'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $installedRoot $name) -PathType Leaf)) {
            throw "Required application-local MSVC runtime is missing: $name"
        }
    }

    if (-not $SkipServiceChecks) {
        if ($env:GITHUB_ACTIONS -ne 'true' -and $env:DESKFLOW_ALLOW_SERVICE_TESTS -ne '1') {
            throw 'Service lifecycle tests require a disposable GitHub runner or DESKFLOW_ALLOW_SERVICE_TESTS=1.'
        }
        $null = Assert-RunningService 'after MSI installation'
        Stop-Service -Name $ServiceName
        $controller = Get-Service -Name $ServiceName
        $controller.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,
            [TimeSpan]::FromSeconds($TimeoutSeconds))
        $report.service_checks += [ordered]@{ step = 'stop'; state = 'Stopped' }
        Start-Service -Name $ServiceName
        $null = Assert-RunningService 'start after stop'
        Restart-Service -Name $ServiceName
        $serviceProcessId = Assert-RunningService 'restart'
        # Catch a daemon that briefly reports RUNNING then crashes or is silently
        # respawned by the service recovery configuration.
        Start-Sleep -Seconds 3
        $stableProcessId = Assert-RunningService 'stable after restart'
        if ($stableProcessId -ne $serviceProcessId) {
            throw 'The service crashed and was respawned after reporting RUNNING.'
        }
        $process = Get-Process -Id $stableProcessId
        try {
            $report.service_modules = @($process.Modules | ForEach-Object {
                Get-BinaryRecord $_.FileName
            })
        } finally { $process.Dispose() }
        foreach ($module in $report.service_modules) {
            $modulePath = [IO.Path]::GetFullPath($module.path)
            if ([IO.Path]::GetFileName($modulePath) -match '^(msvcp140|vcruntime140).*\.dll$' -and
                -not $modulePath.StartsWith($installedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "The service borrowed a system MSVC runtime instead of its packaged copy: $modulePath"
            }
            if (-not $modulePath.StartsWith($installedRoot + '\', [StringComparison]::OrdinalIgnoreCase) -and
                -not $modulePath.StartsWith($env:SystemRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "Service loaded a dependency outside the package and Windows: $modulePath"
            }
        }
        if (-not @($report.service_modules | Where-Object {
                [IO.Path]::GetFileName($_.path) -ieq 'vcruntime140.dll'
            }).Count -or -not @($report.service_modules | Where-Object {
                [IO.Path]::GetFileName($_.path) -ieq 'msvcp140.dll'
            }).Count) {
            throw 'The service module inventory is missing the expected MSVC runtime modules.'
        }
    }
    if ($PortableZip) {
        $portableDirectory = Join-Path $scratch 'portable'
        Expand-Archive -LiteralPath $PortableZip -DestinationPath $portableDirectory
        $candidates = @(Get-ChildItem -LiteralPath $portableDirectory -Recurse -Filter 'deskflow.exe' -File)
        if ($candidates.Count -ne 1) { throw 'Expected exactly one portable Deskflow executable.' }
        $portableRoot = $candidates[0].DirectoryName
        foreach ($name in @('deskflow.exe', 'deskflow-core.exe')) {
            $report.portable_executables += Invoke-CleanVersion $portableRoot $name $true
        }
    }
    $report.passed = $true
    Write-Host 'Windows installed executables, service lifecycle, and packaged runtime verified.'
} catch {
    $report['error'] = $_.Exception.ToString()
    try {
        $report['recent_service_events'] = @(Get-WinEvent -FilterHashtable @{
            LogName = 'System'; ProviderName = 'Service Control Manager'; StartTime = (Get-Date).AddMinutes(-10)
        } -MaxEvents 40 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match [regex]::Escape($ServiceName) } |
            Select-Object TimeCreated, Id, LevelDisplayName, Message)
    } catch { $report['event_collection_error'] = $_.Exception.Message }
    throw
} finally {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $outputPath -Encoding utf8
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
