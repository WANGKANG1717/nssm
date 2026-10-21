[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
  [Parameter(Mandatory = $true)][string]$Archive,
  [Parameter(Mandatory = $true)][string]$Sha256File,
  [string]$InstallDirectory = $PSScriptRoot,
  [string]$ExpectedSignerSubject = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$hashPath = (Resolve-Path -LiteralPath $Sha256File).Path
$installPath = [IO.Path]::GetFullPath($InstallDirectory)
$targetPath = Join-Path $installPath 'svp.exe'
if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
  throw "Existing svp.exe not found: $targetPath"
}

$expectedHash = ((Get-Content -LiteralPath $hashPath -Raw).Trim() -split '\s+')[0]
if ($expectedHash -notmatch '^[A-Fa-f0-9]{64}$') { throw 'Invalid SHA-256 file.' }
$actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash) { throw 'Release archive SHA-256 mismatch.' }

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("SvcPilot-Update-" + [guid]::NewGuid().ToString('N'))
try {
  [void](New-Item -ItemType Directory -Path $temporary)
  Expand-Archive -LiteralPath $archivePath -DestinationPath $temporary
  $candidates = @(Get-ChildItem -LiteralPath $temporary -Filter svp.exe -File -Recurse)
  if ($candidates.Count -ne 1) { throw 'Release archive must contain exactly one svp.exe.' }
  $candidate = $candidates[0].FullName
  $signature = Get-AuthenticodeSignature -LiteralPath $candidate
  if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid) {
    throw "Release executable signature is $($signature.Status), expected Valid."
  }
  if (-not [string]::IsNullOrWhiteSpace($ExpectedSignerSubject) -and
      $signature.SignerCertificate.Subject -ne $ExpectedSignerSubject) {
    throw "Unexpected signer: $($signature.SignerCertificate.Subject)"
  }
  $current = [Diagnostics.FileVersionInfo]::GetVersionInfo($targetPath)
  $incoming = [Diagnostics.FileVersionInfo]::GetVersionInfo($candidate)
  $currentVersion = [version]$current.FileVersion
  $incomingVersion = [version]$incoming.FileVersion
  if ($incomingVersion -le $currentVersion) {
    throw "Incoming version $incomingVersion is not newer than $currentVersion."
  }

  # An in-use image cannot be replaced reliably. This check does not stop or
  # mutate any service; the operator remains in control of the maintenance window.
  $inUse = @(Get-CimInstance Win32_Service | Where-Object {
    $_.State -ne 'Stopped' -and $_.PathName -and
    $_.PathName.Trim('"').StartsWith($targetPath, [StringComparison]::OrdinalIgnoreCase)
  })
  if ($inUse.Count) {
    throw "Stop services using this svp.exe first: $($inUse.Name -join ', ')"
  }

  $backupPath = "$targetPath.$currentVersion.bak"
  if ($PSCmdlet.ShouldProcess($targetPath, "Upgrade SvcPilot to $incomingVersion")) {
    Copy-Item -LiteralPath $targetPath -Destination $backupPath -Force
    $staged = "$targetPath.new"
    Copy-Item -LiteralPath $candidate -Destination $staged -Force
    Move-Item -LiteralPath $staged -Destination $targetPath -Force
    Write-Host "Updated SvcPilot $currentVersion -> $incomingVersion"
    Write-Host "Backup: $backupPath"
  }
}
finally {
  if (Test-Path -LiteralPath $temporary) {
    Remove-Item -LiteralPath $temporary -Recurse -Force
  }
}
