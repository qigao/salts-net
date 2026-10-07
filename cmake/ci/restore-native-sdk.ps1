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
    <PackageReference Include="Salts.Native" Version="*" />
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
'@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM

dotnet restore $project --packages $packages --configfile $config --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw 'Failed to restore the latest Salts and re2c packages' }

# NuGet's resolved graph is authoritative even when the payload cache holds older releases.
$assets = Get-Content -LiteralPath (Join-Path $restoreRoot 'obj/project.assets.json') -Raw | ConvertFrom-Json -AsHashtable
function Get-RestoredPackage([string]$name) {
  $keys = @($assets.libraries.Keys | Where-Object { $_.StartsWith("$name/", [StringComparison]::OrdinalIgnoreCase) })
  if ($keys.Count -ne 1) { throw "Expected one resolved $name package" }
  return Join-Path $packages $assets.libraries[$keys[0]].path
}

$saltsPackage = Get-RestoredPackage 'Salts.Native'
$saltsRoot = Join-Path $saltsPackage "sdk/$SaltsRid"
$saltsHostRoot = Join-Path $saltsPackage "sdk/$HostRid"
$re2cPackage = Get-RestoredPackage 'Qigao.Re2c.Binary'
$re2cRoot = Join-Path $re2cPackage "tools/$HostRid"
$re2cName = if ($IsWindows) { 're2c.exe' } else { 're2c' }
$re2cExecutable = Join-Path $re2cRoot "bin/$re2cName"
foreach ($path in @(
  (Join-Path $saltsRoot 'lib/cmake/Salts/SaltsConfig.cmake'),
  (Join-Path $saltsHostRoot 'lib/cmake/Salts/SaltsConfig.cmake'),
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
  SALTS_VERSION = (Split-Path $saltsPackage -Leaf)
  RE2C_ROOT = $re2cRoot
  RE2C_VERSION = (Split-Path $re2cPackage -Leaf)
}
foreach ($entry in $resolvedEnvironment.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
  if (-not $Local) { "$($entry.Key)=$($entry.Value)" >> $env:GITHUB_ENV }
}
if (-not $Local) { (Join-Path $re2cRoot 'bin') >> $env:GITHUB_PATH }
Write-Host "Restored Salts.Native $env:SALTS_VERSION for $SaltsRid and Qigao.Re2c.Binary $env:RE2C_VERSION for $HostRid"
