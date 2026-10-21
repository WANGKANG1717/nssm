[CmdletBinding()]
param(
  [string]$Executable = '',
  [ValidateRange(1, 1000)][int]$Iterations = 25,
  [ValidateRange(1, 168)][int]$SoakHours = 1,
  [switch]$IUnderstandThisMustRunInDisposableVM
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if (-not $IUnderstandThisMustRunInDisposableVM) {
  throw 'Refusing to run. Use only in a disposable VM and pass -IUnderstandThisMustRunInDisposableVM.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated PowerShell session.'
}
if ([string]::IsNullOrWhiteSpace($Executable)) {
  $Executable = Join-Path $PSScriptRoot 'svp.exe'
}
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$acceptancePath = Join-Path $PSScriptRoot 'Test-SvcPilot.ps1'
if (-not (Test-Path -LiteralPath $acceptancePath -PathType Leaf)) {
  throw "Acceptance script not found: $acceptancePath"
}

$started = [DateTimeOffset]::Now
$results = [Collections.Generic.List[object]]::new()
try {
  for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
    Write-Host ("Acceptance iteration {0}/{1}" -f $iteration, $Iterations) -ForegroundColor Cyan
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $acceptancePath -Executable $executablePath
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    $results.Add([pscustomobject]@{
      Iteration = $iteration
      ExitCode = $exitCode
      DurationSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
    })
    if ($exitCode -ne 0) {
      throw "Acceptance iteration $iteration failed with exit code $exitCode."
    }
  }

  if ($SoakHours -gt 0) {
    $deadline = [DateTimeOffset]::Now.AddHours($SoakHours)
    while ([DateTimeOffset]::Now -lt $deadline) {
      # Re-run the complete isolated acceptance flow. Each pass creates random
      # exact-name services and proves cleanup, exercising SCM churn without
      # touching pre-existing services.
      & $acceptancePath -Executable $executablePath
      if ($LASTEXITCODE -ne 0) { throw 'Soak acceptance pass failed.' }
    }
  }
}
finally {
  $report = [ordered]@{
    schema = 1
    startedAt = $started.ToString('o')
    finishedAt = [DateTimeOffset]::Now.ToString('o')
    executable = $executablePath
    sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
    iterations = $results
  }
  $reportPath = Join-Path $PSScriptRoot 'SvcPilot-Reliability-Report.json'
  $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding utf8
  Write-Host "Reliability report: $reportPath"
}

Write-Host 'PASS: reliability and soak acceptance completed without residual test services.' -ForegroundColor Green
