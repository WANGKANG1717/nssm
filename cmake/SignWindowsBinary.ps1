param(
  [Parameter(Mandatory = $true)][string]$Executable,
  [Parameter(Mandatory = $true)][string]$CertificateBase64,
  [Parameter(Mandatory = $true)][string]$CertificatePassword,
  [string]$ExpectedSubject = ''
)

$ErrorActionPreference = 'Stop'
$signtool = (Get-Command signtool.exe -ErrorAction Stop).Source
$certificate = Join-Path $env:RUNNER_TEMP 'svcpilot-signing.pfx'
try {
  [IO.File]::WriteAllBytes($certificate, [Convert]::FromBase64String($CertificateBase64))
  & $signtool sign /fd SHA256 /td SHA256 /tr https://timestamp.digicert.com `
    /f $certificate /p $CertificatePassword $Executable
  if ($LASTEXITCODE -ne 0) { throw "signtool failed with exit code $LASTEXITCODE" }
  & $signtool verify /pa /v $Executable
  if ($LASTEXITCODE -ne 0) { throw "signature verification failed with exit code $LASTEXITCODE" }
  $signature = Get-AuthenticodeSignature -LiteralPath $Executable
  if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid) {
    throw "Authenticode status is $($signature.Status), expected Valid"
  }
  if (-not [string]::IsNullOrWhiteSpace($ExpectedSubject) -and
      $signature.SignerCertificate.Subject -ne $ExpectedSubject) {
    throw "Unexpected signing subject: $($signature.SignerCertificate.Subject)"
  }
}
finally {
  if (Test-Path -LiteralPath $certificate) { Remove-Item -LiteralPath $certificate -Force }
}
