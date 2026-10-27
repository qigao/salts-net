param(
  [Parameter(Mandatory = $true)][string]$SaltsRid,
  [Parameter(Mandatory = $true)][string]$HostRid,
  [switch]$Local
)
$ErrorActionPreference = 'Stop'

$requiredEnvironment = @('GITHUB_TOKEN')
if (-not $Local) { $requiredEnvironment += @('GITHUB_ENV', 'GITHUB_PATH') }
foreach ($name in $requiredEnvironment) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "$name is required"
  }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$restoreRoot = Join-Path $repositoryRoot 'build/native-sdk'
$packages = Join-Path $repositoryRoot 'stage/github-packages'
$client = Join-Path $repositoryRoot 'build/vcpkg-cache-contract/scripts/github-packages.py'
if (-not (Test-Path -LiteralPath $client -PathType Leaf)) {
  throw "Pinned shared curl NuGet V3 client checkout is missing"
}
$env:GH_TOKEN = $env:GITHUB_TOKEN
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null
New-Item -ItemType Directory -Path $packages -Force | Out-Null

function Get-OfficialNativeVersion([string]$repository, [string]$line,
                                   [string]$packageName) {
  $url = "https://api.github.com/repos/qigao/$repository/releases?per_page=100"
  $headers = @{
    Accept = 'application/vnd.github+json'
    Authorization = "Bearer $env:GITHUB_TOKEN"
    'User-Agent' = 'saltsnet-native-sdk-qualification'
  }
  # Do not wrap Invoke-RestMethod in @(): on pwsh it may become a nested array.
  $releasePayload = Invoke-RestMethod -Method Get -Uri $url -Headers $headers
  $rcPattern = '^v' + [regex]::Escape($line) + '-rc\.([0-9]+)$'
  $candidates = @(
    foreach ($release in $releasePayload) {
      if ($release.draft) { continue }
      $tag = [string]$release.tag_name
      if ($tag -eq "v$line" -and -not $release.prerelease) {
        $priority = [int]::MaxValue
      } elseif ($release.prerelease -and $tag -match $rcPattern) {
        $priority = [int]$Matches[1]
      } else {
        continue
      }
      [pscustomobject]@{ Priority = $priority; Version = $tag.Substring(1); Release = $release }
    }
  )
  if ($candidates.Count -eq 0) {
    throw "No published stable or numeric RC native release for $line in qigao/$repository"
  }
  $selected = $candidates | Sort-Object Priority -Descending | Select-Object -First 1
  $asset = "$packageName.$($selected.Version).nupkg"
  if (@($selected.Release.assets | Where-Object { $_.name -eq $asset }).Count -ne 1) {
    throw "Latest published release $($selected.Release.tag_name) lacks asset $asset"
  }
  return $selected.Version
}

$saltsOfficialVersion = Get-OfficialNativeVersion 'salts' '3.0.0' 'Salts.Native'
$saltsUtilsOfficialVersion = Get-OfficialNativeVersion 'salts-utils' '4.3.0' 'SaltsUtils.Native'
Write-Host "Qualifying official Salts.Native $saltsOfficialVersion + SaltsUtils.Native $saltsUtilsOfficialVersion"
function Restore-QualifiedSdk([string]$Id, [string]$Version, [string]$Repository) {
  $archive = Join-Path $restoreRoot "$Id.$Version.nuget.nupkg"
  $asset = Join-Path $restoreRoot "$Id.$Version.release.nupkg"
  $tag = "v$Version"
  & python $client package $Id $Version $archive | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "curl NuGet V3 download failed: $Id/$Version" }
  # The tag asset is an independent immutable publication and must contain
  # exactly the same package bytes as the GitHub Packages version.
  & python $client release "qigao/$Repository" $tag "$Id.$Version.nupkg" $asset | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "curl GitHub Release asset failed: $Id/$Version" }
  $archiveSha = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
  $releaseSha = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash
  if ($archiveSha -ne $releaseSha) {
    throw "GitHub Release and GitHub Packages payload digests disagree: $Id/$Version"
  }
  $path = Join-Path $packages "$($Id.ToLowerInvariant())/$Version"
  if (Test-Path -LiteralPath $path) {
    Remove-Item -LiteralPath $path -Recurse -Force
  }
  & python $client extract $archive $path | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "native SDK archive extraction failed: $Id/$Version" }
  return $path
}
$saltsPackage = Restore-QualifiedSdk 'Salts.Native' $saltsOfficialVersion 'salts'
$saltsUtilsPackage = Restore-QualifiedSdk 'SaltsUtils.Native' $saltsUtilsOfficialVersion 'salts-utils'
$saltsVersion = $saltsOfficialVersion
$saltsUtilsVersion = $saltsUtilsOfficialVersion

$saltsRoot = Join-Path $saltsPackage "sdk/$SaltsRid"
$saltsHostRoot = Join-Path $saltsPackage "sdk/$HostRid"
$saltsUtilsRoot = Join-Path $saltsUtilsPackage "sdk/$SaltsRid"
$saltsUtilsHostRoot = Join-Path $saltsUtilsPackage "sdk/$HostRid"
$re2cRoot = $env:RE2C_ROOT
$re2cVersion = $env:RE2C_PACKAGE_VERSION
if (-not $re2cRoot -or -not $re2cVersion) {
  throw "Pinned curl-only re2c setup action must provide a verified binary"
}
$re2cName = if ($IsWindows) { 're2c.exe' } else { 're2c' }
$re2cExecutable = Join-Path $re2cRoot "bin/$re2cName"
foreach ($path in @(
  (Join-Path $saltsRoot 'lib/cmake/Salts/SaltsConfig.cmake'),
  (Join-Path $saltsHostRoot 'lib/cmake/Salts/SaltsConfig.cmake'),
  (Join-Path $saltsUtilsRoot 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake'),
  (Join-Path $saltsUtilsHostRoot 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake'),
  $re2cExecutable
)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing restored SDK file: $path" }
}
if (-not $IsWindows) {
  chmod +x $re2cExecutable
  if ($LASTEXITCODE -ne 0) { throw 'Cannot mark restored re2c executable' }
}

$resolvedEnvironment = [ordered]@{
  SALTS_ROOT = $saltsRoot
  SALTS_HOST_ROOT = $saltsHostRoot
  SALTS_VERSION = $saltsVersion
  SALTS_UTILS_ROOT = $saltsUtilsRoot
  SALTS_UTILS_HOST_ROOT = $saltsUtilsHostRoot
  SALTS_UTILS_VERSION = $saltsUtilsVersion
  RE2C_ROOT = $re2cRoot
  RE2C_VERSION = $re2cVersion
}
foreach ($entry in $resolvedEnvironment.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
  if (-not $Local) { "$($entry.Key)=$($entry.Value)" >> $env:GITHUB_ENV }
}
if (-not $Local) { (Join-Path $re2cRoot 'bin') >> $env:GITHUB_PATH }
Write-Host "curl-validated official Salts.Native $saltsVersion and SaltsUtils.Native $saltsUtilsVersion for $SaltsRid, plus re2c $env:RE2C_VERSION for $HostRid"
