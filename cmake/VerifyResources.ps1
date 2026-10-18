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
  (New-UnicodeString @(0x004E, 0x0053, 0x0053, 0x004D, 0x0020, 0x670D, 0x52A1, 0x5B89, 0x88C5, 0x5668)),
  (New-UnicodeString @(0x670D, 0x52A1, 0x540D, 0x79F0, 0xFF1A)),
  (New-UnicodeString @(0x8F93, 0x51FA, 0x0020, 0x0028, 0x0073, 0x0074, 0x0064, 0x006F, 0x0075, 0x0074, 0x0029, 0xFF1A)),
  (New-UnicodeString @(0x66FF, 0x6362, 0x5DF2, 0x5B58, 0x5728, 0x7684, 0x8F93, 0x51FA, 0x002F, 0x9519, 0x8BEF, 0x6587, 0x4EF6)),
  (New-UnicodeString @(0x91CD, 0x5B9A, 0x5411, 0x94A9, 0x5B50, 0x7684, 0x8F93, 0x51FA))
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
