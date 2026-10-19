param(
  [Parameter(Mandatory = $true)]
  [string]$Executable
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'The SCM integration test must run from an elevated administrator session.'
}

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$serviceName = "svcpilot-integration-$PID"
$unmanagedServiceName = "$serviceName-unmanaged"
$testRoot = Join-Path $env:ProgramData $serviceName
$configPath = Join-Path $testRoot 'service.toml'
$exportPath = Join-Path $testRoot 'exported.toml'
$unsafeRoot = Join-Path $env:ProgramData "$serviceName-unsafe"
$secureLeaf = Join-Path $unsafeRoot 'secure'
$unsafeConfigPath = Join-Path $secureLeaf 'service.toml'

function ConvertTo-TomlString([string]$Value) {
  $escaped = $Value.Replace('\', '\\').Replace('"', '\"')
  $escaped = $escaped.Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
  return '"' + $escaped + '"'
}

function Write-TestConfiguration([string]$Path, [bool]$FastFailure) {
  $systemDirectory = [Environment]::SystemDirectory
  $applicationPath = if ($FastFailure) {
    Join-Path $systemDirectory 'cmd.exe'
  } else {
    Join-Path $systemDirectory 'ping.exe'
  }
  $arguments = if ($FastFailure) {
    'arguments = ["/d", "/c", "exit 7"]'
  } else {
    'arguments = ["-t", "127.0.0.1"]'
  }
  $lines = @(
    'Schema = 1',
    '',
    '[Unit]',
    ('Name = {0}' -f (ConvertTo-TomlString $serviceName)),
    'DisplayName = "SvcPilot integration test"',
    '',
    '[Service]',
    ('ExecStart = {0}' -f (ConvertTo-TomlString $applicationPath)),
    $arguments,
    ('WorkingDirectory = {0}' -f (ConvertTo-TomlString $systemDirectory)),
    '',
    '[Windows]',
    'Startup = "manual"',
    'Account = "virtual"',
    'ServiceSid = "unrestricted"'
  )
  if ($FastFailure) {
    $lines[($lines.IndexOf($arguments))] = 'Arguments = ["/d", "/c", "exit 7"]'
    $serviceIndex = $lines.IndexOf('[Service]') + 1
    $lines = @($lines[0..($serviceIndex + 2)] + 'Restart = "never"' +
      $lines[($serviceIndex + 3)..($lines.Count - 1)])
  } else {
    $lines[($lines.IndexOf($arguments))] = 'Arguments = ["-t", "127.0.0.1"]'
    $lines += @(
      '',
      '[Health]',
      'Type = "process"',
      'IntervalSec = "500ms"',
      'TimeoutSec = "500ms"'
    )
  }
  $lines | Set-Content -LiteralPath $Path -Encoding utf8
}

function Set-SecureTestAcl([string]$Path, [bool]$Directory) {
  $acl = New-Object Security.AccessControl.DirectorySecurity
  if (-not $Directory) {
    $acl = New-Object Security.AccessControl.FileSecurity
  }
  $acl.SetAccessRuleProtection($true, $false)
  $acl.SetOwner([Security.Principal.NTAccount]::new('BUILTIN', 'Administrators'))
  $inheritance = if ($Directory) {
    [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
  } else {
    [Security.AccessControl.InheritanceFlags]::None
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

function Invoke-SvpJson([string[]]$Arguments) {
  $text = & $executablePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "svp failed with exit code $LASTEXITCODE`: $text"
  }
  return $text | ConvertFrom-Json
}

try {
  [void](New-Item -ItemType Directory -Path $testRoot -Force)
  Set-SecureTestAcl $testRoot $true
  $systemDirectory = [Environment]::SystemDirectory
  $unmanagedCommand = '"{0}" /c exit 0' -f (Join-Path $systemDirectory 'cmd.exe')
  & sc.exe create $unmanagedServiceName 'binPath=' $unmanagedCommand `
    'start=' 'demand' | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'unmanaged test service creation failed' }
  $foreignParametersKey =
    "HKLM\SYSTEM\CurrentControlSet\Services\$unmanagedServiceName\Parameters"
  & reg.exe add $foreignParametersKey /v ConfigPath /t REG_EXPAND_SZ `
    /d "$env:ProgramData\ForeignService" /f | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'foreign ConfigPath setup failed' }
  & $executablePath remove $unmanagedServiceName confirm 2>$null | Out-Null
  if ($LASTEXITCODE -eq 0) { throw 'svp removed an unmanaged service' }
  & sc.exe query $unmanagedServiceName 2>$null | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'unmanaged service no longer exists' }

  Write-TestConfiguration $configPath $false
  Set-SecureTestAcl $configPath $false

  [void](New-Item -ItemType Directory -Path $secureLeaf -Force)
  $unsafeAcl = New-Object Security.AccessControl.DirectorySecurity
  $unsafeAcl.SetAccessRuleProtection($true, $false)
  $unsafeAcl.SetOwner(
    [Security.Principal.NTAccount]::new('BUILTIN', 'Administrators'))
  foreach ($identity in @('SYSTEM', 'BUILTIN\Administrators', 'Everyone')) {
    $rule = New-Object Security.AccessControl.FileSystemAccessRule(
      $identity, 'FullControl', 'ContainerInherit, ObjectInherit', 'None', 'Allow')
    [void]$unsafeAcl.AddAccessRule($rule)
  }
  Set-Acl -LiteralPath $unsafeRoot -AclObject $unsafeAcl
  Set-SecureTestAcl $secureLeaf $true
  Write-TestConfiguration $unsafeConfigPath $false
  Set-SecureTestAcl $unsafeConfigPath $false
  & $executablePath plan --json $unsafeConfigPath 2>$null | Out-Null
  if ($LASTEXITCODE -ne 6) {
    throw 'configuration beneath an insecure ancestor was not rejected'
  }

  $applied = Invoke-SvpJson @('apply', '--json', $configPath)
  if (-not $applied.ok) { throw 'apply did not report success' }
  $managedServices = @(& $executablePath list)
  if ($LASTEXITCODE -ne 0 -or $managedServices -notcontains $serviceName) {
    throw 'declaratively managed service was missing from svp list'
  }
  if ($managedServices -contains $unmanagedServiceName) {
    throw 'foreign ConfigPath was misidentified as a SvcPilot registration'
  }
  $listed = Invoke-SvpJson @('list', '--json')
  if (-not $listed.ok -or $listed.operation -ne 'list' -or
      $listed.services.name -notcontains $serviceName) {
    throw 'JSON service listing did not include the managed service'
  }
  $verboseServices = @(& $executablePath list --verbose)
  if ($LASTEXITCODE -ne 0 -or
      ($verboseServices -join "`n") -notmatch
        "stopped\s+0\s+$([regex]::Escape($serviceName))") {
    throw 'verbose service listing did not expose stopped state and PID'
  }
  & $executablePath start $serviceName | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'service start failed' }

  $deadline = [DateTime]::UtcNow.AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 250
    $status = Invoke-SvpJson @('status', '--json', $serviceName)
  } while (($status.state -ne 'running' -or -not $status.runtimeAvailable) -and
           [DateTime]::UtcNow -lt $deadline)
  if ($status.state -ne 'running' -or $status.childPid -eq 0) {
    throw 'runtime status did not expose a running child process'
  }
  $processTree = @(& $executablePath processes $serviceName)
  if ($LASTEXITCODE -ne 0 -or
      -not ($processTree -match "\b$($status.childPid)\b")) {
    throw 'process-tree query did not include the managed child process'
  }

  $exported = Invoke-SvpJson @('export', '--json', $serviceName, $exportPath)
  if (-not $exported.ok -or -not (Test-Path -LiteralPath $exportPath)) {
    throw 'configuration export failed'
  }
  & $executablePath stop $serviceName | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'service stop failed' }

  Write-TestConfiguration $configPath $true
  Set-SecureTestAcl $configPath $false
  $updated = Invoke-SvpJson @('apply', '--json', $configPath)
  if (-not $updated.ok) { throw 'fast-failure configuration update failed' }

  $failureStarted = [DateTime]::UtcNow
  & $executablePath start $serviceName 2>$null | Out-Null
  $startExitCode = $LASTEXITCODE
  $failureElapsed = ([DateTime]::UtcNow - $failureStarted).TotalSeconds
  if ($startExitCode -eq 0) {
    throw 'a service which exited during startup was reported as running'
  }
  if ($failureElapsed -gt 5) {
    throw "fast startup failure took $failureElapsed seconds to report"
  }

  & $executablePath remove $serviceName confirm | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'service removal failed' }
}
finally {
  & sc.exe stop $serviceName 2>$null | Out-Null
  & sc.exe delete $serviceName 2>$null | Out-Null
  & sc.exe stop $unmanagedServiceName 2>$null | Out-Null
  & sc.exe delete $unmanagedServiceName 2>$null | Out-Null
  if (Test-Path -LiteralPath $testRoot) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
  }
  if (Test-Path -LiteralPath $unsafeRoot) {
    Remove-Item -LiteralPath $unsafeRoot -Recurse -Force
  }
}
