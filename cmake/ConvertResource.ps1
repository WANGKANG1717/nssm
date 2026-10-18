param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,

  [Parameter(Mandatory = $true)]
  [string]$OutputPath
)

$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
$source = [System.IO.File]::ReadAllText($InputPath, $utf8)
$stringPattern = '"(?:\\.|[^"\\])*"'
$escapedSource = [System.Text.RegularExpressions.Regex]::Replace(
  $source,
  $stringPattern,
  [System.Text.RegularExpressions.MatchEvaluator] {
    param($match)

    if (-not ($match.Value.ToCharArray() | Where-Object { [int]$_ -gt 0x7f })) {
      return $match.Value
    }

    $wideString = [System.Text.StringBuilder]::new($match.Value.Length)
    [void]$wideString.Append('L"')
    foreach ($character in $match.Value.Substring(1, $match.Value.Length - 2).ToCharArray()) {
      $codeUnit = [int]$character
      if ($codeUnit -le 0x7f) {
        [void]$wideString.Append($character)
      }
      else {
        # In an explicitly wide resource string, windres retains the complete
        # four-digit UTF-16 escape instead of truncating it to one byte.
        [void]$wideString.AppendFormat('\x{0:x4}', $codeUnit)
      }
    }
    [void]$wideString.Append('"')
    return $wideString.ToString()
  }
)

# Non-ASCII text outside string literals can only be comments in this source.
# Keep the generated preprocessor input strictly ASCII.
$escaped = [System.Text.RegularExpressions.Regex]::Replace(
  $escapedSource,
  '[^\x00-\x7f]',
  '?'
)

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
  [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
}

[System.IO.File]::WriteAllText(
  $OutputPath,
  $escaped,
  [System.Text.Encoding]::ASCII
)
