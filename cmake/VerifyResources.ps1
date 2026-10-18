param(
  [Parameter(Mandatory = $true)]
  [string]$Executable
)

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
  (New-UnicodeString @(0x7A33, 0x5B9A, 0x53EF, 0x9760, 0x7684, 0x670D, 0x52A1, 0x7BA1, 0x7406, 0x5668)),
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
