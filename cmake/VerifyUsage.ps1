param(
  [Parameter(Mandatory = $true)][string]$Executable,
  [Parameter(Mandatory = $true)][string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
$output = (& $Executable 2>&1 | Out-String)
$exitCode = $LASTEXITCODE

if ($exitCode -ne 1) {
  throw "Usage command returned $exitCode instead of 1"
}
if (-not $output.Contains('SvcPilot')) {
  throw 'Usage output does not contain the SvcPilot product name'
}
if (-not $output.Contains("$ExpectedVersion 64-bit")) {
  throw "Usage output does not contain the expected version: $output"
}
