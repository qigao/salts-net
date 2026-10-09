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
$packages = Join-Path $repositoryRoot 'stage/nuget'
$config = Join-Path $repositoryRoot 'cmake/vcpkg-cache.nuget.config'
$project = Join-Path $restoreRoot 'native-sdk.csproj'
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null

@'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="2.3.0-*" />
    <PackageReference Include="SaltsUtils.Native" Version="4.3.0-*" />
    <!-- The CI release qualification may select the newest published RC tag
         (not unqualified Linux-only rc.sha snapshots in the same feed). -->
    <PackageReference Update="Salts.Native" Version="$(SaltsNativeQualifiedVersion)"
                      Condition="'$(SaltsNativeQualifiedVersion)' != ''" />
    <PackageReference Update="SaltsUtils.Native" Version="$(SaltsUtilsNativeQualifiedVersion)"
                      Condition="'$(SaltsUtilsNativeQualifiedVersion)' != ''" />
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
'@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM

# Choose the newest *officially released* prerelease, not arbitrary internal
# rc.sha qualification snapshots which may contain Linux-only SDK payloads.
# Both PackageReference declarations remain floating 2.3.0-*/4.3.0-*;
# these MSBuild overrides are resolved afresh from the GitHub Release channel.
function Get-OfficialNativeRc([string]$repository, [string]$tagPattern,
                              [string]$packageName) {
  $url = "https://api.github.com/repos/qigao/$repository/releases?per_page=100"
  $headers = @{
    Accept = 'application/vnd.github+json'
    Authorization = "Bearer $env:GITHUB_TOKEN"
    'User-Agent' = 'saltsnet-native-rc-qualification'
  }
  # Invoke-RestMethod already returns the JSON list as an array. Wrapping
  # it in @() can keep that array as one nested pipeline object.
  $releasePayload = Invoke-RestMethod -Method Get -Uri $url -Headers $headers
  $candidates = @(
    foreach ($release in $releasePayload) {
      if ($release.draft -or -not $release.prerelease) { continue }
      if ([string]$release.tag_name -match $tagPattern) {
        $ordinal = [int]$Matches[1]
        [pscustomobject]@{ Ordinal = $ordinal; Version = $release.tag_name.Substring(1); Release = $release }
      }
    }
  )
  if ($candidates.Count -eq 0) {
    throw "No published prerelease tag matching $tagPattern in qigao/$repository"
  }
  $selected = $candidates | Sort-Object Ordinal -Descending | Select-Object -First 1
  $asset = "$packageName.$($selected.Version).nupkg"
  if (@($selected.Release.assets | Where-Object { $_.name -eq $asset }).Count -ne 1) {
    throw "Latest published RC $($selected.Release.tag_name) is missing package asset $asset"
  }
  return $selected.Version
}

$saltsOfficialVersion = Get-OfficialNativeRc 'salts' '^v2\.3\.0-rc\.([0-9]+)$' 'Salts.Native'
$saltsUtilsOfficialVersion = Get-OfficialNativeRc 'salts-utils' '^v4\.3\.0-rc\.([0-9]+)$' 'SaltsUtils.Native'
$restoreProperties = @(
  "-p:SaltsNativeQualifiedVersion=$saltsOfficialVersion",
  "-p:SaltsUtilsNativeQualifiedVersion=$saltsUtilsOfficialVersion"
)
Write-Host "Qualifying published RCs: Salts.Native $saltsOfficialVersion + SaltsUtils.Native $saltsUtilsOfficialVersion"

dotnet restore $project --packages $packages --configfile $config --no-cache --force-evaluate @restoreProperties
if ($LASTEXITCODE -ne 0) { throw 'Failed to restore Salts 2.3 / SaltsUtils 4.3 prereleases and re2c' }

# NuGet's resolved graph is authoritative even when the payload cache holds older releases.
$assets = Get-Content -LiteralPath (Join-Path $restoreRoot 'obj/project.assets.json') -Raw | ConvertFrom-Json -AsHashtable
function Get-RestoredPackage([string]$name) {
  $keys = @($assets.libraries.Keys | Where-Object { $_.StartsWith("$name/", [StringComparison]::OrdinalIgnoreCase) })
  if ($keys.Count -ne 1) { throw "Expected one resolved $name package" }
  return Join-Path $packages $assets.libraries[$keys[0]].path
}

$saltsPackage = Get-RestoredPackage 'Salts.Native'
$saltsUtilsPackage = Get-RestoredPackage 'SaltsUtils.Native'
$saltsVersion = Split-Path $saltsPackage -Leaf
$saltsUtilsVersion = Split-Path $saltsUtilsPackage -Leaf
if ($saltsVersion -notmatch '^2\.3\.0-') {
  throw "Salts.Native must resolve a 2.3.0 prerelease, got: $saltsVersion"
}
if ($saltsUtilsVersion -notmatch '^4\.3\.0-') {
  throw "SaltsUtils.Native must resolve a 4.3.0 prerelease, got: $saltsUtilsVersion"
}
if ($saltsVersion -ne $saltsOfficialVersion -or $saltsUtilsVersion -ne $saltsUtilsOfficialVersion) {
  throw "NuGet resolved a different prerelease than the latest published RCs: $saltsVersion / $saltsUtilsVersion"
}
$saltsRoot = Join-Path $saltsPackage "sdk/$SaltsRid"
$saltsHostRoot = Join-Path $saltsPackage "sdk/$HostRid"
$saltsUtilsRoot = Join-Path $saltsUtilsPackage "sdk/$SaltsRid"
$saltsUtilsHostRoot = Join-Path $saltsUtilsPackage "sdk/$HostRid"
$re2cPackage = Get-RestoredPackage 'Qigao.Re2c.Binary'
$re2cRoot = Join-Path $re2cPackage "tools/$HostRid"
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
  RE2C_VERSION = (Split-Path $re2cPackage -Leaf)
}
foreach ($entry in $resolvedEnvironment.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
  if (-not $Local) { "$($entry.Key)=$($entry.Value)" >> $env:GITHUB_ENV }
}
if (-not $Local) { (Join-Path $re2cRoot 'bin') >> $env:GITHUB_PATH }
Write-Host "Resolved Salts.Native $saltsVersion and SaltsUtils.Native $saltsUtilsVersion for $SaltsRid, plus Qigao.Re2c.Binary $env:RE2C_VERSION for $HostRid"
