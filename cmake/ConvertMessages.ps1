param(
    [Parameter(Mandatory = $true)]
    [string] $InputPath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'

$utf8 = [System.Text.UTF8Encoding]::new($true)
$utf16LeWithoutBom = [System.Text.UnicodeEncoding]::new($false, $false, $true)
$content = [System.IO.File]::ReadAllText($InputPath, $utf8)
[System.IO.File]::WriteAllText($OutputPath, $content, $utf16LeWithoutBom)
