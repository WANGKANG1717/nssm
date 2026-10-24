<#
.SYNOPSIS
  Runs destructive SvcPilot acceptance tests in a disposable Windows VM.

.DESCRIPTION
  Creates uniquely named Windows services, exercises all non-interactive
  SvcPilot features, and removes every test service and temporary directory
  in a finally block. The exit code is the authoritative pass/fail result:
  0 means every step and cleanup check succeeded; any other value is failure.

  Never run this script on a production host. Use an administrator shell in a
  Windows 10/11 x64 VM with a restorable snapshot.

.PARAMETER Executable
  Path to svp.exe. By default the script looks beside itself and then in
  out\Release\win64\svp.exe relative to the source tree.

.EXAMPLE
  Set-ExecutionPolicy -Scope Process Bypass
  .\Test-SvcPilot.ps1

.EXAMPLE
  .\scripts\Test-SvcPilot.ps1 -Executable .\out\Release\win64\svp.exe
#>
[CmdletBinding()]
param(
  [string]$Executable = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:Passed = 0
$script:Step = 0
$script:ExecutablePath = ''
$script:ServiceName = ''
$script:ForeignServiceName = ''
$script:CleanupServiceNames = [Collections.Generic.List[string]]::new()
$script:BackgroundProcesses = [Collections.Generic.List[object]]::new()
$script:FollowProcess = $null
$script:TestRoot = ''
$script:UnsafeRoot = ''
$script:DiagnosticPath = ''

function Write-Step([string]$Name) {
  $script:Step++
  Write-Host ("[{0:00}] {1}" -f $script:Step, $Name) -ForegroundColor Cyan
}

function Complete-Step {
  $script:Passed++
}

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function Register-TestServiceName([string]$Name) {
  if (-not $script:CleanupServiceNames.Contains($Name)) {
    $script:CleanupServiceNames.Add($Name)
  }
}

function New-TestServiceName([string]$Suffix) {
  $name = "$($script:ServiceName)-$Suffix"
  Register-TestServiceName $name
  return $name
}

function ConvertTo-TomlString([string]$Value) {
  $escaped = $Value.Replace('\', '\\').Replace('"', '\"')
  $escaped = $escaped.Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
  return '"' + $escaped + '"'
}

function ConvertTo-TomlStringArray([string[]]$Values) {
  $encoded = @($Values | ForEach-Object { ConvertTo-TomlString $_ })
  return '[' + ($encoded -join ', ') + ']'
}

function Invoke-Native([string]$File, [string[]]$Arguments) {
  $previousPreference = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $text = (& $File @Arguments 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
  }
  finally {
    $ErrorActionPreference = $previousPreference
  }
  return [pscustomobject]@{ ExitCode = $exitCode; Output = $text.Trim() }
}

function Invoke-Svp([string[]]$Arguments, [int[]]$ExpectedExitCodes = @(0)) {
  $result = Invoke-Native $script:ExecutablePath $Arguments
  if ($ExpectedExitCodes -notcontains $result.ExitCode) {
    throw "svp $($Arguments -join ' ') returned $($result.ExitCode), expected $($ExpectedExitCodes -join '/'). Output: $($result.Output)"
  }
  return $result
}

function Invoke-SvpJson([string[]]$Arguments) {
  $result = Invoke-Svp $Arguments
  try {
    $parsed = $result.Output | ConvertFrom-Json
  }
  catch {
    throw "svp $($Arguments -join ' ') output was not valid JSON: $($result.Output)"
  }
  if ($null -eq $parsed) {
    throw "svp $($Arguments -join ' ') returned empty JSON: $($result.Output)"
  }
  $parsed | Add-Member -NotePropertyName '_svpCommand' `
    -NotePropertyValue ($Arguments -join ' ') -Force
  $parsed | Add-Member -NotePropertyName '_svpRawJson' `
    -NotePropertyValue $result.Output -Force
  return $parsed
}

function Get-JsonProperty($Object, [string]$Name) {
  if ($null -eq $Object) { throw "Cannot read JSON property '$Name' from null." }
  $property = $Object.PSObject.Properties[$Name]
  if ($null -eq $property) {
    $names = @($Object.PSObject.Properties.Name | Where-Object { -not $_.StartsWith('_svp') }) -join ', '
    $command = $Object.PSObject.Properties['_svpCommand'].Value
    $raw = $Object.PSObject.Properties['_svpRawJson'].Value
    throw "svp $command JSON is missing '$Name'. Fields: [$names]. Raw JSON: $raw"
  }
  return $property.Value
}

function Set-SecureAcl([string]$Path, [bool]$Directory) {
  if ($Directory) {
    $acl = New-Object Security.AccessControl.DirectorySecurity
  }
  else {
    $acl = New-Object Security.AccessControl.FileSecurity
  }
  $acl.SetAccessRuleProtection($true, $false)
  $acl.SetOwner([Security.Principal.NTAccount]::new('BUILTIN', 'Administrators'))
  if ($Directory) {
    $inheritance = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
  }
  else {
    $inheritance = [Security.AccessControl.InheritanceFlags]::None
  }
  $propagation = [Security.AccessControl.PropagationFlags]::None
  foreach ($identity in @('SYSTEM', 'BUILTIN\Administrators')) {
    $rule = New-Object Security.AccessControl.FileSystemAccessRule(
      $identity, 'FullControl', $inheritance, $propagation, 'Allow')
    [void]$acl.AddAccessRule($rule)
  }
  $readRule = New-Object Security.AccessControl.FileSystemAccessRule(
    'Authenticated Users', 'ReadAndExecute', $inheritance, $propagation, 'Allow')
  [void]$acl.AddAccessRule($readRule)
  Set-Acl -LiteralPath $Path -AclObject $acl
}

function Write-ServiceConfig([string]$Path, [bool]$FailImmediately,
                             [string]$Description) {
  $systemDirectory = [Environment]::SystemDirectory
  $cmd = Join-Path $systemDirectory 'cmd.exe'
  $logPath = Join-Path $script:TestRoot 'service.log'
  $workloadPath = Join-Path $script:TestRoot 'workload.cmd'
  $startPre = Join-Path $script:TestRoot 'start-pre.txt'
  $startPost = Join-Path $script:TestRoot 'start-post.txt'
  $stop = Join-Path $script:TestRoot 'stop.txt'
  $stopPost = Join-Path $script:TestRoot 'stop-post.txt'
  $reload = Join-Path $script:TestRoot 'reload.txt'
  $environmentProbe = Join-Path $script:TestRoot 'environment.txt'

  if ($FailImmediately) {
    $application = $cmd
    $arguments = 'Arguments = ["/d", "/c", "exit 7"]'
  }
  else {
    @(
      '@echo off',
      ('if not exist "{0}" echo %SVP_ACCEPTANCE_TEST%^|%CD%>"{0}"' -f
        $environmentProbe),
      'for /l %%i in (1,1,1000) do @echo SVP_LOG_PROBE_BURST_%%i',
      ':loop',
      'echo SVP_LOG_PROBE',
      'ping.exe -n 2 127.0.0.1 >nul',
      'goto loop'
    ) | Set-Content -LiteralPath $workloadPath -Encoding ascii
    Set-SecureAcl $workloadPath $false
    $application = $cmd
    $arguments = 'Arguments = ["/d", "/q", "/c", {0}]' -f `
      (ConvertTo-TomlString $workloadPath)
  }

  $lines = @(
    '# Generated by the SvcPilot VM acceptance test.',
    'Schema = 1',
    '',
    '[Unit]',
    ('Name = {0}' -f (ConvertTo-TomlString $script:ServiceName)),
    ('DisplayName = {0}' -f (ConvertTo-TomlString "SvcPilot VM test $($script:ServiceName)")),
    ('Description = {0}' -f (ConvertTo-TomlString $Description)),
    '',
    '[Service]',
    ('ExecStart = {0}' -f (ConvertTo-TomlString $application)),
    $arguments,
    ('WorkingDirectory = {0}' -f (ConvertTo-TomlString $systemDirectory)),
    'Environment = ["SVP_ACCEPTANCE_TEST=1", "SVP_UNICODE=中文"]',
    'Restart = "never"',
    'TimeoutStartSec = "20s"',
    'TimeoutStopSec = "3s"',
    'MemoryMax = 268435456',
    'TasksMax = 8',
    'CPUQuota = 100',
    ('ExecStartPre = [{{ Path = {0}, Arguments = ["/d", "/c", {1}] }}]' -f
      (ConvertTo-TomlString $cmd), (ConvertTo-TomlString "echo start-pre>$startPre")),
    ('ExecStartPost = [{{ Path = {0}, Arguments = ["/d", "/c", {1}] }}]' -f
      (ConvertTo-TomlString $cmd), (ConvertTo-TomlString "echo start-post>$startPost")),
    ('ExecStop = [{{ Path = {0}, Arguments = ["/d", "/c", {1}] }}]' -f
      (ConvertTo-TomlString $cmd), (ConvertTo-TomlString "echo stop>$stop")),
    ('ExecStopPost = [{{ Path = {0}, Arguments = ["/d", "/c", {1}] }}]' -f
      (ConvertTo-TomlString $cmd), (ConvertTo-TomlString "echo stop-post>$stopPost")),
    ('ExecReload = [{{ Path = {0}, Arguments = ["/d", "/c", {1}] }}]' -f
      (ConvertTo-TomlString $cmd), (ConvertTo-TomlString "echo reload>$reload")),
    '',
    '[Windows]',
    'Account = "system"',
    'ServiceSid = "unrestricted"',
    'CPUMonitorInterval = "500ms"',
    'CPUViolationThreshold = 10'
  )

  if (-not $FailImmediately) {
    $lines += @(
      '',
      '[Readiness]',
      'Type = "process"',
      'StartupTimeoutSec = "10s"',
      'SuccessThreshold = 1',
      '',
      '[Health]',
      'Type = "process"',
      'InitialDelaySec = "500ms"',
      'IntervalSec = "500ms"',
      'TimeoutSec = "500ms"',
      'FailureThreshold = 3',
      'SuccessThreshold = 1',
      '',
      '[Logging]',
      'Timestamp = true',
      '',
      '[Logging.Stdout]',
      ('Path = {0}' -f (ConvertTo-TomlString $logPath)),
      '',
      '[Logging.Stderr]',
      ('Path = {0}' -f (ConvertTo-TomlString $logPath)),
      '',
      '[Logging.Rotation]',
      'MaximumBytes = 32768'
    )
  }

  $lines | Set-Content -LiteralPath $Path -Encoding utf8
  Set-SecureAcl $Path $false
}

function Write-StandardServiceConfig([string]$Path, [string]$Name,
                                     [string]$ArgumentsToml,
                                     [string]$ApplicationPath = '',
                                     [string]$RestartMode = 'never',
                                     [string[]]$UnitLines = @(),
                                     [string[]]$ServiceLines = @(),
                                     [string]$Account = 'system',
                                     [string]$ServiceSid = 'unrestricted',
                                     [string[]]$WindowsLines = @(),
                                     [string[]]$Sections = @()) {
  $systemDirectory = [Environment]::SystemDirectory
  $cmd = Join-Path $systemDirectory 'cmd.exe'
  if ([string]::IsNullOrWhiteSpace($ApplicationPath)) {
    $ApplicationPath = $cmd
  }
  $lines = @(
    'Schema = 1',
    '',
    '[Unit]',
    ('Name = {0}' -f (ConvertTo-TomlString $Name)),
    ('Description = {0}' -f (ConvertTo-TomlString "SvcPilot VM feature test $Name")),
    $UnitLines,
    '',
    '[Service]',
    ('ExecStart = {0}' -f (ConvertTo-TomlString $ApplicationPath)),
    $ArgumentsToml,
    ('WorkingDirectory = {0}' -f (ConvertTo-TomlString $systemDirectory)),
    ('Restart = {0}' -f (ConvertTo-TomlString $RestartMode)),
    $ServiceLines,
    '',
    '[Windows]',
    ('Account = {0}' -f (ConvertTo-TomlString $Account)),
    ('ServiceSid = {0}' -f (ConvertTo-TomlString $ServiceSid)),
    'CPUMonitorInterval = "100ms"',
    'CPUViolationThreshold = 2',
    $WindowsLines,
    $Sections
  ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  $lines | Set-Content -LiteralPath $Path -Encoding utf8
  Set-SecureAcl $Path $false
}

function Write-NotifyWorkload([string]$Path) {
  $scriptText = @'
$pipePath = $env:SVP_NOTIFY_PIPE
if ([string]::IsNullOrWhiteSpace($pipePath)) { exit 21 }
$pipeName = $pipePath -replace '^\\\\\.\\pipe\\', ''
try {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.', $pipeName, [System.IO.Pipes.PipeDirection]::Out)
  $pipe.Connect(5000)
  $writer = New-Object System.IO.StreamWriter($pipe)
  $writer.AutoFlush = $true
  $writer.WriteLine('READY=1')
  if ($env:SVP_TEST_HEARTBEATS -ne '0') {
    for ($index = 0; $index -lt 20; $index++) {
      $writer.WriteLine('WATCHDOG=1')
      Start-Sleep -Milliseconds 500
    }
    $writer.WriteLine('WATCHDOG=1')
  }
  while ($true) { Start-Sleep -Seconds 5 }
}
catch { exit 22 }
'@
  $scriptText | Set-Content -LiteralPath $Path -Encoding utf8
  Set-SecureAcl $Path $false
}

function Write-CpuLoadWorkload([string]$Path) {
  $scriptText = @'
$workers = @()
for ($index = 0; $index -lt 8; $index++) {
  $workers += Start-Job -ScriptBlock { while ($true) {} }
}
while ($true) { Start-Sleep -Seconds 5 }
'@
  $scriptText | Set-Content -LiteralPath $Path -Encoding utf8
  Set-SecureAcl $Path $false
}

function Write-ProcessLimitWorkload([string]$Path) {
  $scriptText = @'
$ErrorActionPreference = 'Stop'
$workers = @()
for ($index = 0; $index -lt 4; $index++) {
  try {
    $workers += Start-Job -ScriptBlock { while ($true) { Start-Sleep -Seconds 5 } }
  }
  catch {
    # TasksMax may reject creation. The supervisor must still remain healthy.
  }
}
while ($true) { Start-Sleep -Seconds 5 }
'@
  $scriptText | Set-Content -LiteralPath $Path -Encoding utf8
  Set-SecureAcl $Path $false
}

function Get-FreeTcpPort {
  $listener = New-Object Net.Sockets.TcpListener(
    [Net.IPAddress]::Loopback, 0)
  $listener.Start()
  try { return ([Net.IPEndPoint]$listener.LocalEndpoint).Port }
  finally { $listener.Stop() }
}

function Wait-TcpPort([int]$Port, [int]$TimeoutSeconds = 10) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $client = New-Object Net.Sockets.TcpClient
    try {
      $client.Connect('127.0.0.1', $Port)
      return
    }
    catch { Start-Sleep -Milliseconds 100 }
    finally { $client.Dispose() }
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "probe listener did not start on 127.0.0.1:$Port"
}

function Start-ProbeHost([string]$Path, [string[]]$Arguments) {
  $powershellPath = Join-Path ([Environment]::SystemDirectory) `
    'WindowsPowerShell\v1.0\powershell.exe'
  $processArguments = @(
    '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-File', ('"{0}"' -f $Path)
  ) + $Arguments
  $process = Start-Process -FilePath $powershellPath `
    -ArgumentList $processArguments -WindowStyle Hidden -PassThru
  $script:BackgroundProcesses.Add($process)
  return $process
}

function Stop-BackgroundProcesses {
  foreach ($process in $script:BackgroundProcesses) {
    try {
      if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
      [void]$process.WaitForExit(5000)
    }
    catch {
      # Cleanup is best effort; the final SCM and directory checks still fail
      # the acceptance run if an untracked resource remains.
    }
  }
  $script:BackgroundProcesses.Clear()
}

function Start-TcpProbeServer([int]$Port) {
  $scriptPath = Join-Path $script:TestRoot "tcp-probe-$Port.ps1"
  @'
param([int]$Port)
$listener = New-Object Net.Sockets.TcpListener(
  [Net.IPAddress]::Loopback, $Port)
$listener.Start(50)
try {
  while ($true) {
    $client = $listener.AcceptTcpClient()
    $client.Dispose()
  }
}
finally { $listener.Stop() }
'@ | Set-Content -LiteralPath $scriptPath -Encoding utf8
  Set-SecureAcl $scriptPath $false
  [void](Start-ProbeHost $scriptPath @('-Port', [string]$Port))
  Wait-TcpPort $Port
}

function Start-HttpProbeServer([int]$Port) {
  $scriptPath = Join-Path $script:TestRoot "http-probe-$Port.ps1"
  @'
param([int]$Port)
$listener = New-Object Net.Sockets.TcpListener(
  [Net.IPAddress]::Loopback, $Port)
$listener.Start(50)
$response = [Text.Encoding]::ASCII.GetBytes(
  "HTTP/1.1 204 No Content`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
try {
  while ($true) {
    $client = $listener.AcceptTcpClient()
    try {
      $stream = $client.GetStream()
      $buffer = New-Object byte[] 1024
      [void]$stream.Read($buffer, 0, $buffer.Length)
      $stream.Write($response, 0, $response.Length)
      $stream.Flush()
    }
    catch {
      # The readiness preflight may connect and disconnect without an HTTP
      # request. Keep the listener alive for the real probe.
    }
    finally { $client.Dispose() }
  }
}
finally { $listener.Stop() }
'@ | Set-Content -LiteralPath $scriptPath -Encoding utf8
  Set-SecureAcl $scriptPath $false
  [void](Start-ProbeHost $scriptPath @('-Port', [string]$Port))
  Wait-TcpPort $Port
}

function Wait-NamedServiceState([string]$Name, [string]$ExpectedState,
                                [int]$TimeoutSeconds) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $status = Invoke-SvpJson @('status', '--output=json', $Name)
    $state = Get-JsonProperty $status 'state'
    if ($state -eq $ExpectedState) { return $status }
    Start-Sleep -Milliseconds 250
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "$Name did not reach state '$ExpectedState'; last state was '$state'"
}

function Wait-NamedServicesStopped([string[]]$Names, [int]$TimeoutSeconds) {
  foreach ($name in $Names) {
    [void](Wait-NamedServiceState $name 'stopped' $TimeoutSeconds)
  }
}

function Wait-ServiceState([string]$ExpectedState, [int]$TimeoutSeconds) {
  return Wait-NamedServiceState $script:ServiceName $ExpectedState $TimeoutSeconds
}

function Wait-NamedServiceFailure([string]$Name, [int]$TimeoutSeconds = 15) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $status = Invoke-SvpJson @('status', '--output=json', $Name)
    if (Get-JsonProperty $status 'failed') { return $status }
    Start-Sleep -Milliseconds 250
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "$Name did not enter the persistent failed state"
}

function Invoke-LogsFollow([string]$ServiceName) {
  $stdoutPath = Join-Path $script:TestRoot 'logs-follow.out'
  $stderrPath = Join-Path $script:TestRoot 'logs-follow.err'
  $script:FollowProcess = Start-Process -FilePath $script:ExecutablePath `
    -ArgumentList @('logs', '-f', $ServiceName) -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
  try {
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
      if ((Test-Path -LiteralPath $stdoutPath) -and
          (Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue) -match
            'SVP_LOG_PROBE') { return }
      Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    $details = ''
    if (Test-Path -LiteralPath $stdoutPath) {
      $details = Get-Content -LiteralPath $stdoutPath -Raw
    }
    throw "logs --follow produced no expected output. stdout: $details"
  }
  finally {
    if ($script:FollowProcess -and -not $script:FollowProcess.HasExited) {
      Stop-Process -Id $script:FollowProcess.Id -Force -ErrorAction SilentlyContinue
      [void]$script:FollowProcess.WaitForExit(5000)
    }
    $script:FollowProcess = $null
  }
}

function Remove-ExactTestService([string]$Name) {
  if ([string]::IsNullOrWhiteSpace($Name)) { return $true }
  $previousPreference = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & sc.exe stop $Name 2>$null | Out-Null
    & sc.exe delete $Name 2>$null | Out-Null
  }
  finally {
    $ErrorActionPreference = $previousPreference
  }
  $deadline = [DateTime]::UtcNow.AddSeconds(30)
  do {
    if (Test-ServiceAbsent $Name) { return $true }
    Start-Sleep -Milliseconds 250
  } while ([DateTime]::UtcNow -lt $deadline)
  return $false
}

function Test-ServiceAbsent([string]$Name) {
  $result = Invoke-Native 'sc.exe' @('query', $Name)
  return $result.ExitCode -ne 0
}

function Save-FailureDiagnostics([string]$Message) {
  if ([string]::IsNullOrWhiteSpace($script:DiagnosticPath)) { return }
  $lines = [Collections.Generic.List[string]]::new()
  $lines.Add('SvcPilot VM acceptance failure')
  $lines.Add("Time: $([DateTime]::Now.ToString('o'))")
  $lines.Add("Message: $Message")
  $lines.Add("Service: $($script:ServiceName)")
  $lines.Add('')
  foreach ($command in @(
      @($script:ExecutablePath, 'show', '--output=json', $script:ServiceName),
      @($script:ExecutablePath, 'status', '--output=json', $script:ServiceName),
      @('sc.exe', 'queryex', $script:ServiceName),
      @('sc.exe', 'qc', $script:ServiceName))) {
    $file = $command[0]
    $arguments = @($command | Select-Object -Skip 1)
    $result = Invoke-Native $file $arguments
    $lines.Add("> $file $($arguments -join ' ')")
    $lines.Add("Exit code: $($result.ExitCode)")
    $lines.Add($result.Output)
    $lines.Add('')
  }
  try {
    $events = Get-WinEvent -FilterHashtable @{
      LogName = 'Application'
      ProviderName = 'SvcPilot'
      StartTime = [DateTime]::Now.AddMinutes(-10)
    } -ErrorAction Stop | Select-Object -First 30 `
      TimeCreated, Id, LevelDisplayName, Message
    $lines.Add('> Recent SvcPilot Application events')
    $lines.Add(($events | Format-List | Out-String).Trim())
  }
  catch {
    $lines.Add("> Recent SvcPilot Application events unavailable: $($_.Exception.Message)")
  }
  $lines | Set-Content -LiteralPath $script:DiagnosticPath -Encoding utf8
}

$failure = $null
try {
  Write-Step 'Preflight: administrator, Windows x64, and executable'
  $principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())
  Assert-True ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) `
    'Run this script from an elevated PowerShell window.'
  Assert-True ([Environment]::Is64BitOperatingSystem) 'SvcPilot requires 64-bit Windows.'
  Assert-True ([Environment]::OSVersion.Version.Major -ge 10) 'SvcPilot requires Windows 10 or newer.'
  Assert-True ($PSVersionTable.PSVersion -ge [version]'5.1') `
    'PowerShell 5.1 or newer is required.'
  if ([string]::IsNullOrWhiteSpace($Executable)) {
    $candidates = @(
      (Join-Path $PSScriptRoot 'svp.exe'),
      (Join-Path (Split-Path $PSScriptRoot -Parent) 'out\Release\win64\svp.exe')
    )
    $Executable = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
  }
  Assert-True (-not [string]::IsNullOrWhiteSpace($Executable)) 'svp.exe was not found. Pass -Executable <path>.'
  $script:ExecutablePath = (Resolve-Path -LiteralPath $Executable).Path
  $version = Invoke-Svp @('version')
  Assert-True ($version.Output -match '^SvcPilot\s+\d+\.\d+\.\d+\s+64-bit') "Unexpected version output: $($version.Output)"
  Assert-True ((Invoke-Svp @('--version')).Output -eq $version.Output) '--version is not an alias of version.'
  $help = Invoke-Svp @('--help')
  Assert-True ($help.Output -match 'svp <' -and
    $help.Output -match 'list\|list-units') 'help output is incomplete.'
  [void](Invoke-Svp @('status') @(2))
  Complete-Step

  $suffix = [Guid]::NewGuid().ToString('N').Substring(0, 12)
  $script:ServiceName = "svp-vm-$suffix"
  $script:ForeignServiceName = "svp-vm-$suffix-foreign"
  Register-TestServiceName $script:ServiceName
  Register-TestServiceName $script:ForeignServiceName
  $dependencyName = New-TestServiceName 'dependency'
  $dependentName = New-TestServiceName 'dependent'
  $reloadFallbackName = New-TestServiceName 'reload-fallback'
  $continueName = New-TestServiceName 'continue'
  $resourceName = New-TestServiceName 'resource'
  $commandProbeName = New-TestServiceName 'probe-command'
  $fileProbeName = New-TestServiceName 'probe-file'
  $tcpProbeName = New-TestServiceName 'probe-tcp'
  $httpProbeName = New-TestServiceName 'probe-http'
  $notifyProbeName = New-TestServiceName 'probe-notify'
  $healthProbeName = New-TestServiceName 'health-file'
  $watchdogTimeoutName = New-TestServiceName 'watchdog-timeout'
  $cpuLimitName = New-TestServiceName 'cpu-limit'
  $tasksLimitName = New-TestServiceName 'tasks-limit'
  $alwaysRestartName = New-TestServiceName 'restart-always'
  $startFailureName = New-TestServiceName 'start-hook-failure'
  $forcedStopName = New-TestServiceName 'forced-stop'
  $virtualAccountName = New-TestServiceName 'virtual-account'
  $script:TestRoot = Join-Path $env:ProgramData "SvcPilotTest-$suffix"
  $script:UnsafeRoot = Join-Path $env:ProgramData "SvcPilotTest-$suffix-unsafe"
  $script:DiagnosticPath = Join-Path ([Environment]::GetFolderPath('Desktop')) `
    "SvcPilot-Test-Failure-$suffix.txt"
  $configPath = Join-Path $script:TestRoot 'service.toml'
  $failureConfigPath = Join-Path $script:TestRoot 'failure.toml'
  $exportPath = Join-Path $script:TestRoot 'exported.toml'
  $invalidPath = Join-Path $script:TestRoot 'invalid.toml'
  $malformedPath = Join-Path $script:TestRoot 'malformed.toml'
  $unknownFieldPath = Join-Path $script:TestRoot 'unknown-field.toml'
  $driftBackupPath = Join-Path $script:TestRoot 'service-before-drift.toml'
  $dependencyConfig = Join-Path $script:TestRoot 'dependency.toml'
  $dependentConfig = Join-Path $script:TestRoot 'dependent.toml'
  $reloadFallbackConfig = Join-Path $script:TestRoot 'reload-fallback.toml'
  $continueConfig = Join-Path $script:TestRoot 'continue.toml'
  $resourceConfig = Join-Path $script:TestRoot 'resource.toml'
  $commandProbeConfig = Join-Path $script:TestRoot 'probe-command.toml'
  $fileProbeConfig = Join-Path $script:TestRoot 'probe-file.toml'
  $tcpProbeConfig = Join-Path $script:TestRoot 'probe-tcp.toml'
  $httpProbeConfig = Join-Path $script:TestRoot 'probe-http.toml'
  $notifyProbeConfig = Join-Path $script:TestRoot 'probe-notify.toml'
  $healthProbeConfig = Join-Path $script:TestRoot 'health-file.toml'
  $watchdogTimeoutConfig = Join-Path $script:TestRoot 'watchdog-timeout.toml'
  $cpuLimitConfig = Join-Path $script:TestRoot 'cpu-limit.toml'
  $tasksLimitConfig = Join-Path $script:TestRoot 'tasks-limit.toml'
  $alwaysRestartConfig = Join-Path $script:TestRoot 'restart-always.toml'
  $startFailureConfig = Join-Path $script:TestRoot 'start-hook-failure.toml'
  $forcedStopConfig = Join-Path $script:TestRoot 'forced-stop.toml'
  $virtualAccountConfig = Join-Path $script:TestRoot 'virtual-account.toml'

  Assert-True (Test-ServiceAbsent $script:ServiceName) 'Random test service already exists.'
  Assert-True (Test-ServiceAbsent $script:ForeignServiceName) 'Random foreign service already exists.'

  Write-Step 'Create isolated, ACL-protected test workspace'
  [void](New-Item -ItemType Directory -Path $script:TestRoot)
  Set-SecureAcl $script:TestRoot $true
  Write-ServiceConfig $configPath $false 'SvcPilot VM acceptance test'
  $pingPath = Join-Path ([Environment]::SystemDirectory) 'ping.exe'
  Write-StandardServiceConfig $dependencyConfig $dependencyName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath
  Write-StandardServiceConfig $dependentConfig $dependentName `
    'Arguments = ["-t", "127.0.0.1"]' `
    -ApplicationPath $pingPath `
    -UnitLines @(('Requires = [{0}]' -f (ConvertTo-TomlString $dependencyName)))
  Write-StandardServiceConfig $reloadFallbackConfig $reloadFallbackName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath
  Write-StandardServiceConfig $continueConfig $continueName `
    'Arguments = ["/d", "/c", "exit 7"]' -RestartMode 'on-failure' -ServiceLines @(
      'RestartSec = "30s"',
      'RestartMaxDelaySec = "30s"',
      'RestartSteps = 1',
      'RestartJitterPercent = 0',
      'StartLimitIntervalSec = "1h"',
      'StartLimitBurst = 100'
    )

  $resourceWorkload = Join-Path $script:TestRoot 'allocate-memory.ps1'
  $resourceScript = @(
    '$blocks = @()',
    'for ($index = 0; $index -lt 3; $index++) {',
    '  $block = New-Object -TypeName byte[] -ArgumentList 67108864',
    '  for ($offset = 0; $offset -lt $block.Length; $offset += 4096) {',
    '    $block[$offset] = 1',
    '  }',
    '  $blocks += ,$block',
    '}',
    'Start-Sleep -Seconds 60'
  ) -join [Environment]::NewLine
  $resourceScript | Set-Content -LiteralPath $resourceWorkload -Encoding utf8
  Set-SecureAcl $resourceWorkload $false
  $powershellPath = Join-Path ([Environment]::SystemDirectory) `
    'WindowsPowerShell\v1.0\powershell.exe'
  $resourceArguments = @(
    '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-File', $resourceWorkload
  ) | ForEach-Object { ConvertTo-TomlString $_ }
  $resourceArgumentsLine = 'Arguments = [' +
    ($resourceArguments -join ', ') + ']'
  Write-StandardServiceConfig $resourceConfig $resourceName $resourceArgumentsLine `
    -ApplicationPath $powershellPath `
    -ServiceLines @(
      'MemoryMax = 134217728',
      'TasksMax = 8'
    )

  $cmdPath = Join-Path ([Environment]::SystemDirectory) 'cmd.exe'
  Write-StandardServiceConfig $commandProbeConfig $commandProbeName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -Sections @(
      '[Readiness]',
      'Type = "command"',
      'StartupTimeoutSec = "10s"',
      'IntervalSec = "100ms"',
      'TimeoutSec = "2s"',
      '[Readiness.Command]',
      ('ExecStart = {0}' -f (ConvertTo-TomlString $cmdPath)),
      'Arguments = ["/d", "/c", "exit 0"]'
    )

  $fileProbeSignal = Join-Path $script:TestRoot 'file-probe.ready'
  'ready' | Set-Content -LiteralPath $fileProbeSignal -Encoding ascii
  Set-SecureAcl $fileProbeSignal $false
  Write-StandardServiceConfig $fileProbeConfig $fileProbeName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -Sections @(
      '[Readiness]',
      'Type = "file"',
      'StartupTimeoutSec = "10s"',
      'IntervalSec = "100ms"',
      'TimeoutSec = "1s"',
      '[Readiness.File]',
      ('Path = {0}' -f (ConvertTo-TomlString $fileProbeSignal)),
      'MaximumAgeSec = "1min"'
    )

  $healthSignal = Join-Path $script:TestRoot 'health.ready'
  'ready' | Set-Content -LiteralPath $healthSignal -Encoding ascii
  Set-SecureAcl $healthSignal $false
  Write-StandardServiceConfig $healthProbeConfig $healthProbeName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -Sections @(
      '[Readiness]',
      'Type = "process"',
      'StartupTimeoutSec = "10s"',
      '[Health]',
      'Type = "file"',
      'InitialDelaySec = "100ms"',
      'IntervalSec = "100ms"',
      'TimeoutSec = "100ms"',
      'FailureThreshold = 2',
      'SuccessThreshold = 1',
      '[Health.File]',
      ('Path = {0}' -f (ConvertTo-TomlString $healthSignal)),
      'MaximumAgeSec = "1min"'
    )

  $notifyWorkload = Join-Path $script:TestRoot 'notify-workload.ps1'
  Write-NotifyWorkload $notifyWorkload
  $notifyArguments = @(
    '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-File', $notifyWorkload
  ) | ForEach-Object { ConvertTo-TomlString $_ }
  Write-StandardServiceConfig $notifyProbeConfig $notifyProbeName `
    ('Arguments = [' + ($notifyArguments -join ', ') + ']') `
    -ApplicationPath $powershellPath -ServiceLines @(
      'Type = "notify"',
      'WatchdogSec = "5s"',
      'TimeoutStartSec = "15s"'
    )

  Write-StandardServiceConfig $watchdogTimeoutConfig $watchdogTimeoutName `
    ('Arguments = [' + ($notifyArguments -join ', ') + ']') `
    -ApplicationPath $powershellPath -ServiceLines @(
      'Type = "notify"',
      'WatchdogSec = "1s"',
      'TimeoutStartSec = "10s"',
      'Environment = ["SVP_TEST_HEARTBEATS=0"]'
    )

  $cpuWorkload = Join-Path $script:TestRoot 'cpu-load.ps1'
  Write-CpuLoadWorkload $cpuWorkload
  $cpuArguments = ConvertTo-TomlStringArray @(
    '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-File', $cpuWorkload
  )
  Write-StandardServiceConfig $cpuLimitConfig $cpuLimitName `
    ('Arguments = {0}' -f $cpuArguments) `
    -ApplicationPath $powershellPath -ServiceLines @(
      'CPUQuota = 1',
      'TimeoutStartSec = "10s"'
    )

  $tasksWorkload = Join-Path $script:TestRoot 'tasks-load.ps1'
  Write-ProcessLimitWorkload $tasksWorkload
  $tasksArguments = ConvertTo-TomlStringArray @(
    '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-File', $tasksWorkload
  )
  Write-StandardServiceConfig $tasksLimitConfig $tasksLimitName `
    ('Arguments = {0}' -f $tasksArguments) `
    -ApplicationPath $powershellPath -ServiceLines @(
      'TasksMax = 2',
      'TimeoutStartSec = "10s"'
    )

  Write-StandardServiceConfig $alwaysRestartConfig $alwaysRestartName `
    'Arguments = ["/d", "/c", "ping.exe -n 2 127.0.0.1 >nul & exit 0"]' `
    -RestartMode 'always' -ServiceLines @(
      'RestartSec = "500ms"',
      'RestartMaxDelaySec = "1s"',
      'RestartSteps = 1',
      'RestartJitterPercent = 0',
      'StartLimitIntervalSec = "1h"',
      'StartLimitBurst = 100'
    )

  $startHookFailureLine = 'ExecStartPre = [{{ Path = {0}, Arguments = ["/d", "/c", "exit 1"] }}]' -f
    (ConvertTo-TomlString $cmdPath)
  Write-StandardServiceConfig $startFailureConfig $startFailureName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -ServiceLines @(
      'TimeoutStartSec = "5s"',
      $startHookFailureLine
    )

  $forcedStopMarker = Join-Path $script:TestRoot 'forced-stop-post.txt'
  $forcedStopAction = 'ExecStop = [{{ Path = {0}, Arguments = ["/d", "/c", "ping.exe -n 6 127.0.0.1 >nul"] }}]' -f
    (ConvertTo-TomlString $cmdPath)
  $forcedStopPostAction = 'ExecStopPost = [{{ Path = {0}, Arguments = ["/d", "/c", {1}] }}]' -f
    (ConvertTo-TomlString $cmdPath),
    (ConvertTo-TomlString "echo stop-post>$forcedStopMarker")
  Write-StandardServiceConfig $forcedStopConfig $forcedStopName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -ServiceLines @(
      'TimeoutStopSec = "2s"',
      $forcedStopAction,
      $forcedStopPostAction
    )

  Write-StandardServiceConfig $virtualAccountConfig $virtualAccountName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -Account 'virtual' `
    -ServiceSid 'restricted' -ServiceLines @('TimeoutStartSec = "10s"')

  foreach ($testConfig in @(
      $configPath, $dependencyConfig, $dependentConfig,
      $reloadFallbackConfig, $continueConfig, $resourceConfig,
      $commandProbeConfig, $fileProbeConfig, $healthProbeConfig,
      $notifyProbeConfig, $watchdogTimeoutConfig, $cpuLimitConfig,
      $tasksLimitConfig, $alwaysRestartConfig, $startFailureConfig,
      $forcedStopConfig, $virtualAccountConfig)) {
    [void](Invoke-SvpJson @('validate', '--output=json', $testConfig))
  }
  Complete-Step

  Write-Step 'Validate strict TOML and reject invalid schema'
  $valid = Invoke-SvpJson @('validate', '--output=json', $configPath)
  Assert-True (Get-JsonProperty $valid 'ok') 'Valid configuration was rejected.'
  @('Schema = 999', '[Unit]', 'Name = "invalid"') |
    Set-Content -LiteralPath $invalidPath -Encoding utf8
  Set-SecureAcl $invalidPath $false
  $invalid = Invoke-Svp @('validate', '--output=json', $invalidPath) @(1)
  Assert-True ($invalid.Output -match 'validation') 'Invalid schema did not produce a validation error.'
  $invalidJson = $invalid.Output | ConvertFrom-Json
  Assert-True ((Get-JsonProperty $invalidJson 'ok') -eq $false -and
    (Get-JsonProperty (Get-JsonProperty $invalidJson 'error') 'code') -eq
      'validation') 'Invalid schema did not return structured JSON.'

  @('Schema = 1', '[Unit', 'Name = "broken"') |
    Set-Content -LiteralPath $malformedPath -Encoding utf8
  Set-SecureAcl $malformedPath $false
  $malformed = Invoke-Svp @('validate', $malformedPath) @(1)
  Assert-True ($malformed.Output -match 'Invalid TOML') 'Malformed TOML was not rejected.'

  $unknownLines = [Collections.Generic.List[string]]::new()
  $unknownLines.AddRange([string[]](Get-Content -LiteralPath $configPath))
  $unitIndex = $unknownLines.IndexOf('[Unit]')
  Assert-True ($unitIndex -ge 0) 'Main configuration has no [Unit] section.'
  $unknownLines.Insert($unitIndex + 1, 'UnexpectedSetting = true')
  $unknownLines | Set-Content -LiteralPath $unknownFieldPath -Encoding utf8
  Set-SecureAcl $unknownFieldPath $false
  $unknown = Invoke-Svp @('validate', $unknownFieldPath) @(1)
  Assert-True ($unknown.Output -match 'unknown|UnexpectedSetting') 'Unknown configuration field was not rejected.'
  Complete-Step

  Write-Step 'Reject configuration below a writable ancestor'
  $unsafeLeaf = Join-Path $script:UnsafeRoot 'secure'
  [void](New-Item -ItemType Directory -Path $unsafeLeaf -Force)
  $unsafeAcl = New-Object Security.AccessControl.DirectorySecurity
  $unsafeAcl.SetAccessRuleProtection($true, $false)
  $unsafeAcl.SetOwner([Security.Principal.NTAccount]::new('BUILTIN', 'Administrators'))
  foreach ($identity in @('SYSTEM', 'BUILTIN\Administrators', 'Everyone')) {
    $rule = New-Object Security.AccessControl.FileSystemAccessRule(
      $identity, 'FullControl', 'ContainerInherit, ObjectInherit', 'None', 'Allow')
    [void]$unsafeAcl.AddAccessRule($rule)
  }
  Set-Acl -LiteralPath $script:UnsafeRoot -AclObject $unsafeAcl
  Set-SecureAcl $unsafeLeaf $true
  $unsafeConfig = Join-Path $unsafeLeaf 'service.toml'
  Copy-Item -LiteralPath $configPath -Destination $unsafeConfig
  Set-SecureAcl $unsafeConfig $false
  $unsafe = Invoke-Svp @('plan', '--output=json', $unsafeConfig) @(6)
  Assert-True ($unsafe.Output -match 'security') 'Unsafe path did not report a security error.'
  Complete-Step

  Write-Step 'Protect unrelated Windows services from removal'
  $commandLine = '"{0}" /d /c exit 0' -f (Join-Path ([Environment]::SystemDirectory) 'cmd.exe')
  $created = Invoke-Native 'sc.exe' @('create', $script:ForeignServiceName, 'binPath=', $commandLine, 'start=', 'demand')
  Assert-True ($created.ExitCode -eq 0) "Unable to create isolated foreign test service: $($created.Output)"
  $foreignKey = "HKLM\SYSTEM\CurrentControlSet\Services\$($script:ForeignServiceName)\Parameters"
  $registered = Invoke-Native 'reg.exe' @('add', $foreignKey, '/v', 'ConfigPath', '/t', 'REG_EXPAND_SZ', '/d', "$env:ProgramData\ForeignService", '/f')
  Assert-True ($registered.ExitCode -eq 0) 'Unable to create foreign registration marker.'
  [void](Invoke-Svp @('remove', $script:ForeignServiceName, 'confirm') @(4))
  Assert-True (-not (Test-ServiceAbsent $script:ForeignServiceName)) 'SvcPilot deleted a service it does not manage.'
  Complete-Step

  Write-Step 'Plan creation, apply service, and verify idempotence'
  $plan = Invoke-SvpJson @('plan', '--output=json', $configPath)
  $planAction = Get-JsonProperty $plan 'action'
  Assert-True ($planAction -eq 'create') "Expected create plan, got $planAction."
  $applied = Invoke-SvpJson @('apply', '--output=json', $configPath)
  Assert-True ((Get-JsonProperty $applied 'ok') -and
    (Get-JsonProperty $applied 'operation') -eq 'create') 'Initial apply failed.'
  $secondPlan = Invoke-SvpJson @('plan', '--output=json', $configPath)
  $secondAction = Get-JsonProperty $secondPlan 'action'
  Assert-True ($secondAction -eq 'noChange') "Second plan was not idempotent: $secondAction."
  Complete-Step

  Write-Step 'Apply multiple configurations in dependency order and batch operations'
  $batch = Invoke-SvpJson @(
    'apply', '--output=json', $dependentConfig, $dependencyConfig
  )
  $orderedServices = @(Get-JsonProperty $batch 'services')
  Assert-True (($orderedServices.Count -eq 2) -and
    ($orderedServices[0] -eq $dependencyName) -and
    ($orderedServices[1] -eq $dependentName)) 'apply did not sort dependencies correctly.'
  $dependencyConfigResult = Invoke-Native 'sc.exe' @('qc', $dependentName)
  Assert-True ($dependencyConfigResult.ExitCode -eq 0 -and
    $dependencyConfigResult.Output -match [regex]::Escape($dependencyName)) `
    'SCM dependency was not registered.'

  [void](Invoke-Svp @('enable', $dependencyName, $dependentName))
  Assert-True ((Invoke-Svp @('is-enabled', $dependencyName)).Output -eq 'enabled') `
    'batch enable failed.'
  [void](Invoke-Svp @('disable', $dependencyName, $dependentName))
  Assert-True ((Invoke-Svp @('is-enabled', $dependentName) @(1)).Output -eq 'disabled') `
    'batch disable failed.'

  [void](Invoke-Svp @('start', $dependencyName, $dependentName))
  [void](Wait-NamedServiceState $dependencyName 'running' 20)
  [void](Wait-NamedServiceState $dependentName 'running' 20)
  $batchActive = Invoke-Svp @('is-active', $dependencyName, $dependentName)
  Assert-True (([regex]::Matches($batchActive.Output, '(?m)^active$')).Count -eq 2) `
    'batch start did not activate both services.'
  [void](Invoke-Svp @('stop', $dependentName, $dependencyName))
  Wait-NamedServicesStopped @($dependentName, $dependencyName) 20
  $batchStates = Invoke-Svp @('is-active', $dependencyName, $dependentName) @(3)
  Assert-True (([regex]::Matches($batchStates.Output, '(?m)^inactive$')).Count -eq 2) `
    'batch stop did not deactivate both services.'

  $cycleA = New-TestServiceName 'cycle-a'
  $cycleB = New-TestServiceName 'cycle-b'
  $cycleAConfig = Join-Path $script:TestRoot 'cycle-a.toml'
  $cycleBConfig = Join-Path $script:TestRoot 'cycle-b.toml'
  Write-StandardServiceConfig $cycleAConfig $cycleA `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -UnitLines @(
      ('Requires = [{0}]' -f (ConvertTo-TomlString $cycleB)))
  Write-StandardServiceConfig $cycleBConfig $cycleB `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -UnitLines @(
      ('Requires = [{0}]' -f (ConvertTo-TomlString $cycleA)))
  [void](Invoke-Svp @('apply', '--output=json', $cycleAConfig, $cycleBConfig) @(4))
  Assert-True ((Test-ServiceAbsent $cycleA) -and (Test-ServiceAbsent $cycleB)) `
    'dependency cycle left a service behind.'
  Complete-Step

  Write-Step 'List service and exercise enable, disable, mask, and unmask'
  $list = Invoke-SvpJson @('list', '-o', 'json')
  $services = Get-JsonProperty $list 'services'
  Assert-True ($services.name -contains $script:ServiceName) 'Managed service is missing from list.'
  $textList = Invoke-Svp @('list')
  Assert-True ($textList.Output -match [regex]::Escape($script:ServiceName)) 'text list omitted the managed service.'
  $unitList = Invoke-Svp @('list-units')
  Assert-True ($unitList.Output -match [regex]::Escape($script:ServiceName)) 'list-units alias failed.'
  $verboseList = Invoke-Svp @('list', '-v')
  Assert-True ($verboseList.Output -match
    ('(stopped|running)\s+\d+\s+' + [regex]::Escape($script:ServiceName))) `
    'verbose list did not expose state and PID.'
  $allList = Invoke-Svp @('list', '-a', '-o', 'json')
  Assert-True ((@(Get-JsonProperty $allList 'services')).Count -gt
    (@($services)).Count) 'list --all did not include native Windows services.'
  $state = Invoke-Svp @('is-enabled', $script:ServiceName) @(1)
  Assert-True ($state.Output -eq 'disabled') 'New service is not manual/disabled-at-boot.'
  [void](Invoke-Svp @('enable', $script:ServiceName))
  Assert-True ((Invoke-Svp @('is-enabled', $script:ServiceName)).Output -eq 'enabled') 'enable failed.'
  [void](Invoke-Svp @('mask', $script:ServiceName))
  Assert-True ((Invoke-Svp @('is-enabled', $script:ServiceName) @(1)).Output -eq 'masked') 'mask failed.'
  [void](Invoke-Svp @('unmask', $script:ServiceName))
  Assert-True ((Invoke-Svp @('is-enabled', $script:ServiceName) @(1)).Output -eq 'disabled') 'unmask failed.'
  [void](Invoke-Svp @('credential', 'delete', $script:ServiceName))
  [void](Invoke-Svp @('credential', 'delete', $script:ServiceName))
  Complete-Step

  Write-Step 'Start service and verify readiness, hooks, runtime status, and process tree'
  [void](Invoke-Svp @('start', $script:ServiceName))
  $running = Wait-ServiceState 'running' 20
  Assert-True ((Get-JsonProperty $running 'runtimeAvailable') -and
    (Get-JsonProperty $running 'childPid') -gt 0) 'Runtime snapshot or child PID is unavailable.'
  Assert-True ((Invoke-Svp @('is-active', $script:ServiceName)).Output -eq 'active') 'is-active did not report active.'
  $quietActive = Invoke-Svp @('is-active', '-q', $script:ServiceName)
  Assert-True ($quietActive.Output -eq '') 'is-active --quiet emitted output.'
  $show = Invoke-SvpJson @('show', '--output=json', $script:ServiceName)
  Assert-True ((Get-JsonProperty $show 'activeState') -eq 'active' -and
    (Get-JsonProperty $show 'mainPid') -eq (Get-JsonProperty $running 'childPid')) 'show returned inconsistent runtime state.'
  $showText = Invoke-Svp @('show', $script:ServiceName)
  Assert-True ($showText.Output -match 'ActiveState=active' -and
    $showText.Output -match 'ConfigDrift=no') 'text show output is incomplete.'
  $mainServiceConfig = Invoke-Native 'sc.exe' @('qc', $script:ServiceName)
  Assert-True ($mainServiceConfig.ExitCode -eq 0 -and
    $mainServiceConfig.Output -match [regex]::Escape(
      "SvcPilot VM test $($script:ServiceName)")) `
    'SCM display name was not applied from TOML.'
  $statusVerbose = Invoke-Svp @('status', '-v', $script:ServiceName)
  Assert-True ($statusVerbose.Output -match 'State: running' -and
    $statusVerbose.Output -match 'Config drift: no') 'verbose status output is incomplete.'
  $mainPid = Get-JsonProperty $running 'childPid'
  $processes = Invoke-Svp @('processes', $script:ServiceName)
  Assert-True ($processes.Output -match "\b$mainPid\b") 'processes did not include the main process.'
  Assert-True (Test-Path -LiteralPath (Join-Path $script:TestRoot 'start-pre.txt')) 'ExecStartPre did not run.'
  Assert-True (Test-Path -LiteralPath (Join-Path $script:TestRoot 'start-post.txt')) 'ExecStartPost did not run.'
  $environmentPath = Join-Path $script:TestRoot 'environment.txt'
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  while (-not (Test-Path -LiteralPath $environmentPath) -and
         [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
  Assert-True (Test-Path -LiteralPath $environmentPath) 'environment/working-directory probe was not created.'
  $environmentText = Get-Content -LiteralPath $environmentPath -Raw
  Assert-True ($environmentText -match '^1\|' -and
    $environmentText -match [regex]::Escape([Environment]::SystemDirectory)) `
    "environment or working directory was not applied: $environmentText"

  $deadline = [DateTime]::UtcNow.AddSeconds(8)
  do {
    Start-Sleep -Milliseconds 250
    $resourceStatus = Invoke-SvpJson @('status', '--output=json', $script:ServiceName)
  } while (((Get-JsonProperty $resourceStatus 'memoryBytes') -le 0 -or
      (Get-JsonProperty $resourceStatus 'processCount') -lt 2) -and
      [DateTime]::UtcNow -lt $deadline)
  Assert-True ((Get-JsonProperty $resourceStatus 'memoryBytes') -gt 0) 'runtime memory accounting is unavailable.'
  Assert-True ((Get-JsonProperty $resourceStatus 'processCount') -ge 2) 'Job Object process accounting did not include descendants.'
  Complete-Step

  Write-Step 'Verify logging, rotation control, reload, and restart'
  $logPath = Join-Path $script:TestRoot 'service.log'
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  while ((-not (Test-Path -LiteralPath $logPath) -or
      (Get-Item -LiteralPath $logPath -ErrorAction SilentlyContinue).Length -eq 0) -and
      [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 250
  }
  Assert-True (Test-Path -LiteralPath $logPath) 'Log file was not created.'
  Assert-True ((Get-Item -LiteralPath $logPath).Length -gt 0) 'Log file received no application output.'
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  do {
    $autoRotated = @(Get-ChildItem -LiteralPath $script:TestRoot -Filter 'service-*.log' -File)
    if ($autoRotated.Count -gt 0) { break }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  Assert-True ($autoRotated.Count -gt 0) 'MaximumBytes did not trigger automatic log rotation.'
  $logs = Invoke-Svp @('logs', '-n', '5', $script:ServiceName)
  Assert-True ($logs.Output -match 'SVP_LOG_PROBE') 'logs did not return the expected ASCII probe.'
  $longLogs = Invoke-Svp @('logs', '--lines', '5', $script:ServiceName)
  Assert-True ($longLogs.Output -match 'SVP_LOG_PROBE') 'long logs option failed.'
  Invoke-LogsFollow $script:ServiceName
  $rotatedBeforeManual = @(
    Get-ChildItem -LiteralPath $script:TestRoot -Filter 'service-*.log' -File).Count
  [void](Invoke-Svp @('rotate', $script:ServiceName))
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  do {
    $rotated = @(Get-ChildItem -LiteralPath $script:TestRoot -Filter 'service-*.log' -File)
    if ($rotated.Count -gt $rotatedBeforeManual) { break }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  Assert-True ($rotated.Count -gt $rotatedBeforeManual -and
    @($rotated | Where-Object Length -gt 0).Count -gt 0) `
    'online log rotation created no new rotated log.'
  [void](Invoke-Svp @('reload', $script:ServiceName))
  $reloadPath = Join-Path $script:TestRoot 'reload.txt'
  $deadline = [DateTime]::UtcNow.AddSeconds(5)
  while (-not (Test-Path -LiteralPath $reloadPath) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
  Assert-True (Test-Path -LiteralPath $reloadPath) 'ExecReload did not run.'
  $oldPid = Get-JsonProperty $running 'childPid'
  [void](Invoke-Svp @('reload-or-restart', $script:ServiceName))
  $afterReloadOrRestart = Wait-ServiceState 'running' 20
  Assert-True ((Get-JsonProperty $afterReloadOrRestart 'childPid') -eq $oldPid) `
    'reload-or-restart unnecessarily restarted a reload-capable service.'
  [void](Invoke-Svp @('restart', $script:ServiceName))
  $running = Wait-ServiceState 'running' 20
  $newPid = Get-JsonProperty $running 'childPid'
  Assert-True ($newPid -gt 0 -and $newPid -ne $oldPid) 'restart did not replace the main process.'
  Complete-Step

  Write-Step 'Verify reload fallback and Windows event query'
  [void](Invoke-SvpJson @('apply', '--output=json', $reloadFallbackConfig))
  [void](Invoke-Svp @('start', $reloadFallbackName))
  $fallbackRunning = Wait-NamedServiceState $reloadFallbackName 'running' 20
  $fallbackPid = Get-JsonProperty $fallbackRunning 'childPid'
  [void](Invoke-Svp @('reload', $reloadFallbackName) @(5))
  [void](Invoke-Svp @('reload-or-restart', $reloadFallbackName))
  $fallbackRestarted = Wait-NamedServiceState $reloadFallbackName 'running' 20
  Assert-True ((Get-JsonProperty $fallbackRestarted 'childPid') -ne $fallbackPid) `
    'reload-or-restart did not restart a service without ExecReload.'

  $events = Invoke-SvpJson @('events', '-n', '20', '-o', 'json', $script:ServiceName)
  Assert-True ((Get-JsonProperty $events 'operation') -eq 'events' -and
    (@(Get-JsonProperty $events 'events')).Count -gt 0) `
    'events returned no service-associated Windows events.'
  $textEvents = Invoke-Svp @('events', '--lines', '5', $script:ServiceName)
  Assert-True ($textEvents.Output -match [regex]::Escape($script:ServiceName)) `
    'text events output omitted the service name.'
  [void](Invoke-Svp @('stop', $reloadFallbackName))
  [void](Wait-NamedServiceState $reloadFallbackName 'stopped' 20)
  Complete-Step

  Write-Step 'Export configuration and verify exported TOML'
  $exported = Invoke-SvpJson @('export', '--output=json', $script:ServiceName, $exportPath)
  Assert-True ((Get-JsonProperty $exported 'ok') -and (Test-Path -LiteralPath $exportPath)) 'export failed.'
  $exportValidation = Invoke-SvpJson @('validate', '--output=json', $exportPath)
  Assert-True (Get-JsonProperty $exportValidation 'ok') 'Exported configuration is invalid.'
  Complete-Step

  Write-Step 'Detect configuration drift and restore the applied fingerprint'
  Copy-Item -LiteralPath $configPath -Destination $driftBackupPath -Force
  Set-SecureAcl $driftBackupPath $false
  $driftLines = @(Get-Content -LiteralPath $configPath)
  $descriptionIndex = -1
  for ($index = 0; $index -lt $driftLines.Count; $index++) {
    if ($driftLines[$index] -match '^Description\s*=') {
      $descriptionIndex = $index
      break
    }
  }
  Assert-True ($descriptionIndex -ge 0) 'Main configuration has no Description field.'
  $driftLines[$descriptionIndex] =
    'Description = "SvcPilot VM acceptance drift probe"'
  $driftLines | Set-Content -LiteralPath $configPath -Encoding utf8
  Set-SecureAcl $configPath $false
  $driftStatus = Invoke-SvpJson @('status', '--output=json', $script:ServiceName)
  Assert-True ((Get-JsonProperty $driftStatus 'configDrift') -and
    (Get-JsonProperty $driftStatus 'restartRequired')) `
    'status did not detect configuration drift and restart requirement.'
  $driftPlan = Invoke-SvpJson @('plan', '--output=json', $configPath)
  Assert-True ((Get-JsonProperty $driftPlan 'action') -eq 'update') `
    'plan did not report the drifted configuration as an update.'
  Copy-Item -LiteralPath $driftBackupPath -Destination $configPath -Force
  Set-SecureAcl $configPath $false
  $restoredStatus = Invoke-SvpJson @('status', '--output=json', $script:ServiceName)
  Assert-True (-not (Get-JsonProperty $restoredStatus 'configDrift') -and
    -not (Get-JsonProperty $restoredStatus 'restartRequired')) `
    'restoring the applied configuration did not clear drift state.'
  Complete-Step

  Write-Step 'Stop service and verify stop hooks and inactive state'
  [void](Invoke-Svp @('stop', $script:ServiceName))
  [void](Wait-ServiceState 'stopped' 15)
  Assert-True ((Invoke-Svp @('is-active', $script:ServiceName) @(3)).Output -eq 'inactive') 'is-active did not report inactive.'
  Assert-True (Test-Path -LiteralPath (Join-Path $script:TestRoot 'stop.txt')) 'ExecStop did not run.'
  Assert-True (Test-Path -LiteralPath (Join-Path $script:TestRoot 'stop-post.txt')) 'ExecStopPost did not run.'
  Complete-Step

  Write-Step 'Exercise command, file, TCP, and HTTP readiness probes'
  foreach ($probe in @(
      @{ Name = $commandProbeName; Config = $commandProbeConfig },
      @{ Name = $fileProbeName; Config = $fileProbeConfig })) {
    [void](Invoke-SvpJson @('apply', '--output=json', $probe.Config))
    [void](Invoke-Svp @('start', $probe.Name))
    [void](Wait-NamedServiceState $probe.Name 'running' 20)
    [void](Invoke-Svp @('stop', $probe.Name))
    [void](Wait-NamedServiceState $probe.Name 'stopped' 15)
  }

  $tcpPort = Get-FreeTcpPort
  Start-TcpProbeServer $tcpPort
  Write-StandardServiceConfig $tcpProbeConfig $tcpProbeName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -Sections @(
      '[Readiness]',
      'Type = "tcp"',
      'StartupTimeoutSec = "10s"',
      'IntervalSec = "100ms"',
      'TimeoutSec = "1s"',
      '[Readiness.TCP]',
      'Host = "127.0.0.1"',
      ('Port = {0}' -f $tcpPort)
    )
  [void](Invoke-SvpJson @('apply', '--output=json', $tcpProbeConfig))
  [void](Invoke-Svp @('start', $tcpProbeName))
  [void](Wait-NamedServiceState $tcpProbeName 'running' 20)
  [void](Invoke-Svp @('stop', $tcpProbeName))
  [void](Wait-NamedServiceState $tcpProbeName 'stopped' 15)

  $httpPort = Get-FreeTcpPort
  Start-HttpProbeServer $httpPort
  Write-StandardServiceConfig $httpProbeConfig $httpProbeName `
    'Arguments = ["-t", "127.0.0.1"]' -ApplicationPath $pingPath -Sections @(
      '[Readiness]',
      'Type = "http"',
      'StartupTimeoutSec = "10s"',
      'IntervalSec = "100ms"',
      'TimeoutSec = "2s"',
      '[Readiness.HTTP]',
      ('URL = "http://127.0.0.1:{0}/ready"' -f $httpPort),
      'MinimumStatus = 200',
      'MaximumStatus = 299'
    )
  [void](Invoke-SvpJson @('apply', '--output=json', $httpProbeConfig))
  [void](Invoke-Svp @('start', $httpProbeName))
  [void](Wait-NamedServiceState $httpProbeName 'running' 20)
  [void](Invoke-Svp @('stop', $httpProbeName))
  [void](Wait-NamedServiceState $httpProbeName 'stopped' 15)
  Complete-Step

  Write-Step 'Exercise notify readiness/watchdog and health failure handling'
  [void](Invoke-SvpJson @('apply', '--output=json', $notifyProbeConfig))
  [void](Invoke-Svp @('start', $notifyProbeName))
  $notifyRunning = Wait-NamedServiceState $notifyProbeName 'running' 25
  Start-Sleep -Seconds 2
  $notifyCurrent = Invoke-SvpJson @('status', '--output=json', $notifyProbeName)
  Assert-True ((Get-JsonProperty $notifyCurrent 'state') -eq 'running' -and
    -not (Get-JsonProperty $notifyCurrent 'failed')) `
    'notify/watchdog service did not remain healthy.'
  [void](Invoke-Svp @('stop', $notifyProbeName))
  [void](Wait-NamedServiceState $notifyProbeName 'stopped' 15)

  [void](Invoke-SvpJson @('apply', '--output=json', $healthProbeConfig))
  [void](Invoke-Svp @('start', $healthProbeName))
  [void](Wait-NamedServiceState $healthProbeName 'running' 20)
  Remove-Item -LiteralPath $healthSignal -Force
  $healthFailure = Wait-NamedServiceFailure $healthProbeName 20
  Assert-True ((Get-JsonProperty $healthFailure 'failureKind') -in
    @('health', 'processExit', 'internal')) `
    'health failure did not preserve a meaningful failure kind.'
  [void](Invoke-Svp @('reset-failed', $healthProbeName))

  [void](Invoke-SvpJson @('apply', '--output=json', $watchdogTimeoutConfig))
  [void](Invoke-Svp @('start', $watchdogTimeoutName) @(0, 5))
  $watchdogFailure = Wait-NamedServiceFailure $watchdogTimeoutName 20
  Assert-True ((Get-JsonProperty $watchdogFailure 'failureKind') -eq
    'watchdog') 'missing WATCHDOG=1 messages did not record a watchdog failure.'
  Assert-True ((Get-JsonProperty $watchdogFailure 'failureMessage') -match
    'watchdog') 'watchdog failure omitted diagnostics.'
  [void](Invoke-Svp @('reset-failed', $watchdogTimeoutName))
  Complete-Step

  Write-Step 'Enforce memory, CPU, and process-count resource limits'
  [void](Invoke-SvpJson @('apply', '--output=json', $resourceConfig))
  [void](Invoke-Svp @('start', $resourceName) @(0, 5))
  $resourceFailure = Wait-NamedServiceFailure $resourceName 20
  Assert-True ((Get-JsonProperty $resourceFailure 'failureKind') -eq
    'resourceMemory') 'memory limit did not record resourceMemory failure.'
  Assert-True ((Get-JsonProperty $resourceFailure 'failureMessage') -match
    'limit') 'resource failure omitted limit diagnostics.'
  [void](Invoke-Svp @('reset-failed', $resourceName))

  [void](Invoke-SvpJson @('apply', '--output=json', $cpuLimitConfig))
  [void](Invoke-Svp @('start', $cpuLimitName) @(0, 5))
  $cpuFailure = Wait-NamedServiceFailure $cpuLimitName 25
  Assert-True ((Get-JsonProperty $cpuFailure 'failureKind') -eq
    'resourceCpu') 'CPUQuota did not record a resourceCpu failure.'

  [void](Invoke-Svp @('reset-failed', $cpuLimitName))
  [void](Invoke-SvpJson @('apply', '--output=json', $tasksLimitConfig))
  [void](Invoke-Svp @('start', $tasksLimitName))
  [void](Wait-NamedServiceState $tasksLimitName 'running' 20)
  Start-Sleep -Seconds 3
  $taskStatus = Invoke-SvpJson @('status', '--output=json', $tasksLimitName)
  $taskCount = Get-JsonProperty $taskStatus 'processCount'
  Assert-True ($taskCount -ge 2 -and $taskCount -le 2) `
    "TasksMax=2 was not enforced; observed $taskCount processes."
  [void](Invoke-Svp @('stop', $tasksLimitName))
  [void](Wait-NamedServiceState $tasksLimitName 'stopped' 20)
  Complete-Step

  Write-Step 'Verify crash recovery backoff and the continue control'
  [void](Invoke-SvpJson @('apply', '--output=json', $continueConfig))
  [void](Invoke-Svp @('start', $continueName) @(0, 5))
  [void](Wait-NamedServiceState $continueName 'paused' 20)
  $beforeContinue = Invoke-SvpJson @('show', '--output=json', $continueName)
  $restartsBefore = Get-JsonProperty $beforeContinue 'nRestarts'
  [void](Invoke-Svp @('continue', $continueName))
  $deadline = [DateTime]::UtcNow.AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 250
    $afterContinue = Invoke-SvpJson @('show', '--output=json', $continueName)
  } while ((Get-JsonProperty $afterContinue 'nRestarts') -le $restartsBefore -and
           [DateTime]::UtcNow -lt $deadline)
  Assert-True ((Get-JsonProperty $afterContinue 'nRestarts') -gt
    $restartsBefore) 'continue did not wake the recovery backoff.'
  [void](Invoke-Svp @('stop', $continueName))
  [void](Wait-NamedServiceState $continueName 'stopped' 20)
  Complete-Step

  Write-Step 'Verify Restart=always and virtual-account isolation'
  [void](Invoke-SvpJson @('apply', '--output=json', $alwaysRestartConfig))
  [void](Invoke-Svp @('start', $alwaysRestartName))
  $deadline = [DateTime]::UtcNow.AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 500
    $alwaysShow = Invoke-SvpJson @('show', '--output=json', $alwaysRestartName)
  } while ((Get-JsonProperty $alwaysShow 'nRestarts') -lt 2 -and
           [DateTime]::UtcNow -lt $deadline)
  Assert-True ((Get-JsonProperty $alwaysShow 'nRestarts') -ge 2) `
    'Restart=always did not restart a cleanly exiting process.'
  [void](Invoke-Svp @('stop', $alwaysRestartName))
  [void](Wait-NamedServiceState $alwaysRestartName 'stopped' 20)

  [void](Invoke-SvpJson @('apply', '--output=json', $virtualAccountConfig))
  $virtualConfig = Invoke-Native 'sc.exe' @('qc', $virtualAccountName)
  Assert-True ($virtualConfig.ExitCode -eq 0 -and
    $virtualConfig.Output -match 'NT SERVICE\\') `
    'virtual service account was not registered as NT SERVICE\<name>.'
  [void](Invoke-Svp @('start', $virtualAccountName))
  $virtualStatus = Wait-NamedServiceState $virtualAccountName 'running' 20
  Assert-True ((Get-JsonProperty $virtualStatus 'runtimeAvailable') -and
    (Get-JsonProperty $virtualStatus 'childPid') -gt 0) `
    'virtual-account service did not publish runtime state.'
  [void](Invoke-Svp @('stop', $virtualAccountName))
  [void](Wait-NamedServiceState $virtualAccountName 'stopped' 20)
  Complete-Step

  Write-Step 'Verify startup-hook failure and forced-stop cleanup'
  [void](Invoke-SvpJson @('apply', '--output=json', $startFailureConfig))
  $hookStartedAt = [DateTime]::UtcNow
  [void](Invoke-Svp @('start', $startFailureName) @(5))
  Assert-True ((([DateTime]::UtcNow - $hookStartedAt).TotalSeconds) -lt 8) `
    'ExecStartPre failure was not reported promptly.'
  $startHookFailure = Wait-NamedServiceFailure $startFailureName 15
  Assert-True ((Get-JsonProperty $startHookFailure 'failureKind') -in
    @('lifecycle', 'startFailure')) `
    'ExecStartPre failure did not preserve lifecycle/start-failure details.'
  [void](Invoke-Svp @('reset-failed', $startFailureName))

  [void](Invoke-SvpJson @('apply', '--output=json', $forcedStopConfig))
  [void](Invoke-Svp @('start', $forcedStopName))
  $forcedRunning = Wait-NamedServiceState $forcedStopName 'running' 20
  $forcedPid = Get-JsonProperty $forcedRunning 'childPid'
  $forcedStartedAt = [DateTime]::UtcNow
  [void](Invoke-Svp @('stop', $forcedStopName))
  $forcedElapsed = ([DateTime]::UtcNow - $forcedStartedAt).TotalSeconds
  Assert-True ($forcedElapsed -ge 2 -and $forcedElapsed -lt 20) `
    "forced stop took an unexpected $forcedElapsed seconds."
  [void](Wait-NamedServiceState $forcedStopName 'stopped' 20)
  $deadline = [DateTime]::UtcNow.AddSeconds(5)
  while (-not (Test-Path -LiteralPath $forcedStopMarker) -and
         [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
  Assert-True (Test-Path -LiteralPath $forcedStopMarker) `
    'ExecStopPost did not run after the stop timeout.'
  Start-Sleep -Milliseconds 500
  Assert-True ($null -eq (Get-Process -Id $forcedPid -ErrorAction SilentlyContinue)) `
    'forced stop left the main process alive.'
  Complete-Step

  Write-Step 'Apply an update and verify fast failure diagnostics'
  Write-ServiceConfig $failureConfigPath $true 'SvcPilot VM fast-failure test'
  $updatePlan = Invoke-SvpJson @('plan', '--output=json', $failureConfigPath)
  Assert-True ((Get-JsonProperty $updatePlan 'action') -eq 'update') 'Changed configuration did not produce an update plan.'
  $updated = Invoke-SvpJson @('apply', '--output=json', $failureConfigPath)
  Assert-True ((Get-JsonProperty $updated 'ok') -and
    (Get-JsonProperty $updated 'operation') -eq 'update') 'Configuration update failed.'
  $startedAt = [DateTime]::UtcNow
  [void](Invoke-Svp @('start', $script:ServiceName) @(5))
  Assert-True ((([DateTime]::UtcNow - $startedAt).TotalSeconds) -lt 8) 'Fast startup failure was not reported promptly.'
  $failed = Invoke-Svp @('is-failed', $script:ServiceName)
  Assert-True ($failed.Output -eq 'failed') 'Persistent failure state was not recorded.'
  $failureShow = Invoke-SvpJson @('show', '--output=json', $script:ServiceName)
  Assert-True ((Get-JsonProperty $failureShow 'failed') -and
    (Get-JsonProperty $failureShow 'execMainStatus') -eq 7) 'Failure exit code 7 was not preserved.'
  [void](Invoke-Svp @('reset-failed', $script:ServiceName))
  Assert-True ((Invoke-Svp @('is-failed', $script:ServiceName) @(1)).Output -eq 'inactive') 'reset-failed did not clear state.'
  Complete-Step

  Write-Step 'Remove service and verify no SCM registration remains'
  foreach ($name in $script:CleanupServiceNames) {
    if ($name -eq $script:ForeignServiceName) { continue }
    if (-not (Test-ServiceAbsent $name)) {
      [void](Invoke-Svp @('remove', $name, 'confirm'))
    }
    Assert-True (Test-ServiceAbsent $name) "Managed test service remains after remove: $name"
  }
  Assert-True (-not (Test-ServiceAbsent $script:ForeignServiceName)) `
    'unmanaged protection fixture disappeared before cleanup.'
  Complete-Step
}
catch {
  $failure = $_
  Save-FailureDiagnostics $failure.Exception.Message
}
finally {
  Write-Host 'Cleaning exact test services and temporary files...' -ForegroundColor DarkGray
  $cleanupErrors = [Collections.Generic.List[string]]::new()
  if ($script:FollowProcess -and -not $script:FollowProcess.HasExited) {
    Stop-Process -Id $script:FollowProcess.Id -Force -ErrorAction SilentlyContinue
  }
  Stop-BackgroundProcesses
  foreach ($name in $script:CleanupServiceNames) {
    if (-not (Remove-ExactTestService $name)) {
      $cleanupErrors.Add("Service remains after cleanup: $name")
    }
    if (-not [string]::IsNullOrWhiteSpace($script:ExecutablePath)) {
      [void](Invoke-Native $script:ExecutablePath @('credential', 'delete', $name))
    }
  }
  foreach ($path in @($script:TestRoot, $script:UnsafeRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-Path -LiteralPath $path)) {
      Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
      if (Test-Path -LiteralPath $path) {
        $cleanupErrors.Add("Directory remains after cleanup: $path")
      }
    }
  }
  if ($cleanupErrors.Count) {
    $cleanupMessage = $cleanupErrors -join '; '
    if ($failure) {
      $cleanupMessage = "$($failure.Exception.Message); cleanup also failed: $cleanupMessage"
    }
    $failure = [Management.Automation.ErrorRecord]::new(
      [InvalidOperationException]::new($cleanupMessage),
      'SvcPilotAcceptanceCleanupFailed',
      [Management.Automation.ErrorCategory]::InvalidResult,
      $null)
  }
}

if ($failure) {
  Write-Host ''
  Write-Host "FAILED after $script:Passed completed steps." -ForegroundColor Red
  Write-Host $failure.Exception.Message -ForegroundColor Red
  if (-not [string]::IsNullOrWhiteSpace($script:DiagnosticPath) -and
      (Test-Path -LiteralPath $script:DiagnosticPath)) {
    Write-Host "Diagnostics: $($script:DiagnosticPath)" -ForegroundColor Yellow
  }
  exit 1
}

Write-Host ''
Write-Host "PASS: all $script:Passed SvcPilot VM acceptance steps completed; cleanup succeeded." -ForegroundColor Green
exit 0
