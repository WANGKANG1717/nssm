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
$serviceName = "nssm-integration-$PID"
$unmanagedServiceName = "$serviceName-unmanaged"
$testRoot = Join-Path $env:ProgramData $serviceName
$configPath = Join-Path $testRoot 'service.json'
$exportPath = Join-Path $testRoot 'exported.json'

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

function Invoke-NssmJson([string[]]$Arguments) {
  $text = & $executablePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "nssm failed with exit code $LASTEXITCODE`: $text"
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
  & $executablePath remove $unmanagedServiceName confirm 2>$null | Out-Null
  if ($LASTEXITCODE -eq 0) { throw 'nssm removed an unmanaged service' }
  & sc.exe query $unmanagedServiceName 2>$null | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'unmanaged service no longer exists' }

  $configuration = @{
    schema = 1
    service = @{
      name = $serviceName
      displayName = 'NSSM integration test'
      startup = 'manual'
      account = @{ type = 'virtual' }
    }
    application = @{
      path = (Join-Path $systemDirectory 'ping.exe')
      arguments = @('-t', '127.0.0.1')
      workingDirectory = $systemDirectory
    }
    health = @{ type = 'process'; intervalMs = 500; timeoutMs = 500 }
    security = @{ serviceSid = 'unrestricted' }
  }
  $configuration | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $configPath -Encoding utf8
  Set-SecureTestAcl $configPath $false

  $applied = Invoke-NssmJson @('apply', '--json', $configPath)
  if (-not $applied.ok) { throw 'apply did not report success' }
  $managedServices = @(& $executablePath list)
  if ($LASTEXITCODE -ne 0 -or $managedServices -notcontains $serviceName) {
    throw 'declaratively managed service was missing from nssm list'
  }
  & $executablePath start $serviceName | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'service start failed' }

  $deadline = [DateTime]::UtcNow.AddSeconds(20)
  do {
    Start-Sleep -Milliseconds 250
    $status = Invoke-NssmJson @('status', '--json', $serviceName)
  } while (($status.state -ne 'running' -or -not $status.runtimeAvailable) -and
           [DateTime]::UtcNow -lt $deadline)
  if ($status.state -ne 'running' -or $status.childPid -eq 0) {
    throw 'runtime status did not expose a running child process'
  }

  $exported = Invoke-NssmJson @('export', '--json', $serviceName, $exportPath)
  if (-not $exported.ok -or -not (Test-Path -LiteralPath $exportPath)) {
    throw 'configuration export failed'
  }
  & $executablePath stop $serviceName | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'service stop failed' }
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
}
