param(
  [Parameter(Mandatory = $true)][string]$Executable,
  [Parameter(Mandatory = $true)][string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'

# PowerShell 7.4+ can promote stderr from a native command with a non-zero
# exit code to a terminating NativeCommandError.  A no-argument invocation is
# expected to write usage to stderr and return 1, so capture that result before
# restoring strict error handling for the assertions below.
$ErrorActionPreference = 'Continue'
$output = (& $Executable 2>&1 | Out-String)
$exitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'

if ($exitCode -ne 1) {
  throw "Usage command returned $exitCode instead of 1"
}
if (-not $output.Contains('SvcPilot')) {
  throw 'Usage output does not contain the SvcPilot product name'
}
if (-not $output.Contains("$ExpectedVersion 64-bit")) {
  throw "Usage output does not contain the expected version: $output"
}
