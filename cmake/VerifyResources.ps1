param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,

  [Parameter(Mandatory = $true)]
  [version]$ExpectedVersion
)

$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
if ($versionInfo.FileMajorPart -ne $ExpectedVersion.Major -or
    $versionInfo.FileMinorPart -ne $ExpectedVersion.Minor -or
    $versionInfo.FileBuildPart -ne $ExpectedVersion.Build -or
    $versionInfo.FilePrivatePart -ne 0) {
  throw "Compiled file version $($versionInfo.FileMajorPart).$($versionInfo.FileMinorPart).$($versionInfo.FileBuildPart).$($versionInfo.FilePrivatePart) does not match $ExpectedVersion.0"
}
if ($versionInfo.ProductName -ne 'SvcPilot') {
  throw "Unexpected product name: $($versionInfo.ProductName)"
}
if ($versionInfo.OriginalFilename -ne 'svp.exe') {
  throw "Unexpected original filename: $($versionInfo.OriginalFilename)"
}
if ($versionInfo.FileDescription -ne 'SvcPilot Windows service supervisor') {
  throw "Unexpected file description: $($versionInfo.FileDescription)"
}

$bytes = [System.IO.File]::ReadAllBytes($Executable)
$utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)

function New-UnicodeString {
  param(
    [Parameter(Mandatory = $true)]
    [int[]]$CodeUnits
  )

  return -join ($CodeUnits | ForEach-Object { [char]$_ })
}

# Keep this script ASCII-only. Windows PowerShell 5.1 treats UTF-8 files without
# a BOM as the active ANSI code page, which would corrupt literal CJK text in CI.
$expected = @(
  (New-UnicodeString @(0x9A8C, 0x8BC1, 0x914D, 0x7F6E)),
  (New-UnicodeString @(0x79FB, 0x9664, 0xFF08, 0x663E, 0x5F0F, 0x786E, 0x8BA4, 0xFF09)),
  (New-UnicodeString @(0x7BA1, 0x7406, 0x5458, 0x6743, 0x9650)),
  (New-UnicodeString @(0x914D, 0x7F6E, 0x6587, 0x4EF6))
)

foreach ($text in $expected) {
  if (-not $utf16.Contains($text)) {
    throw "Missing expected UTF-16 resource string: $text"
  }
}

$mojibake = @(
  (New-UnicodeString @(0x93C8, 0x5D85, 0x59DF)),
  (New-UnicodeString @(0x935A, 0x5D87, 0x041E)),
  (New-UnicodeString @(0x95BD, 0x2541, 0x74D9))
)
foreach ($text in $mojibake) {
  if ($utf16.Contains($text)) {
    throw "Found mojibake in compiled resources: $text"
  }
}
