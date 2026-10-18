param(
  [Parameter(Mandatory = $true)][string]$Executable,
  [Parameter(Mandatory = $true)][string]$OutputDirectory,
  [Parameter(Mandatory = $true)][string]$Version,
  [Parameter(Mandatory = $true)][string]$SourceDirectory
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$stage = Join-Path $output "svcpilot-$Version-win64"
$archive = Join-Path $output "svcpilot-$Version-win64.zip"

[void](New-Item -ItemType Directory -Path $output -Force)
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
[void](New-Item -ItemType Directory -Path $stage)
Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'svp.exe')
Copy-Item -LiteralPath (Join-Path $source 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $source 'THIRD_PARTY_NOTICES.md') -Destination $stage

$binaryHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'svp.exe')).Hash.ToLowerInvariant()
$sbom = [ordered]@{
  spdxVersion = 'SPDX-2.3'
  dataLicense = 'CC0-1.0'
  SPDXID = 'SPDXRef-DOCUMENT'
  name = "svcpilot-$Version-win64"
  documentNamespace = "https://github.com/WANGKANG1717/nssm/releases/download/v$Version/svcpilot-$Version-win64.spdx.json"
  creationInfo = [ordered]@{ creators = @('Tool: SvcPilot-PackageRelease'); created = '1970-01-01T00:00:00Z' }
  packages = @(
    [ordered]@{
      name = 'svcpilot'; SPDXID = 'SPDXRef-Package-SvcPilot'; versionInfo = $Version
      downloadLocation = 'NOASSERTION'; filesAnalyzed = $true
      licenseConcluded = 'CC0-1.0'; licenseDeclared = 'CC0-1.0'
      checksums = @([ordered]@{ algorithm = 'SHA256'; checksumValue = $binaryHash })
    },
    [ordered]@{
      name = 'yyjson'; SPDXID = 'SPDXRef-Package-yyjson'; versionInfo = '0.12.0'
      downloadLocation = 'git+https://github.com/ibireme/yyjson.git@8b4a38dc994a110abaec8a400615567bd996105f'
      filesAnalyzed = $false; licenseConcluded = 'MIT'; licenseDeclared = 'MIT'
    }
  )
  relationships = @([ordered]@{
    spdxElementId = 'SPDXRef-Package-SvcPilot'; relationshipType = 'DEPENDS_ON'
    relatedSpdxElement = 'SPDXRef-Package-yyjson'
  })
}
$sbom | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $stage "svcpilot-$Version-win64.spdx.json") -Encoding utf8
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
"$archiveHash  $([IO.Path]::GetFileName($archive))" |
  Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
