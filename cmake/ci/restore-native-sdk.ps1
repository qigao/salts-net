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
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
'@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM

dotnet restore $project --packages $packages --configfile $config --no-cache --force-evaluate
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
