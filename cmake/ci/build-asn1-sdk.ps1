param(
  [Parameter(Mandatory = $true)][string]$TargetRid,
  [Parameter(Mandatory = $true)][string]$HostRid
)
$ErrorActionPreference = 'Stop'

# A workflow snapshot resolves master once for all six RIDs; no releases.
$cryptoCommit = $env:CRYPTO_SOURCE_SHA
if ([string]::IsNullOrWhiteSpace($cryptoCommit) -or
    $cryptoCommit -cnotmatch '^[0-9a-f]{40}$') {
  throw 'Missing exact resolved Crypto master source SHA'
}
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$upstream = Join-Path $root 'build/crypto-upstream'
$source = Join-Path $upstream 'asn1'
if (-not (Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt') -PathType Leaf)) {
  throw 'Canonical qigao/crypto/asn1 source is missing'
}
$actual = (& git -C $upstream rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $actual -cne $cryptoCommit) {
  throw "Unexpected ASN1 upstream HEAD $actual; required $cryptoCommit"
}
$roots = Join-Path $root 'stage/asn1-sdk'
$builds = Join-Path $root 'build/asn1-sdk'
New-Item -ItemType Directory -Force -Path $roots, $builds | Out-Null

function Invoke-Cmake([string[]]$Arguments, [string]$Context) {
  & cmake @Arguments | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "ASN1 $Context failed for $TargetRid" }
}
function Invoke-CTest([string[]]$Arguments) {
  & ctest @Arguments | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "ASN1 CTest failed for $TargetRid" }
}

function Build-ASN1([string]$Rid, [bool]$RunHostTests) {
  $prefix = Join-Path $roots $Rid
  $build = Join-Path $builds $Rid
  $options = @(
    '-S', $source, '-B', $build,
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
    "-DCMAKE_INSTALL_PREFIX=$prefix",
    "-DBUILD_TESTING=$($RunHostTests ? 'ON' : 'OFF')"
  )
  if ($Rid -eq 'windows-x64') {
    if (-not $IsWindows) { throw "Cannot compile Windows ASN1 on a non-Windows host" }
    $options += @('-G', 'Visual Studio 18 2026', '-A', 'x64')
  } else {
    $options += @('-G', 'Ninja')
    if ($Rid -eq 'android-arm64-v8a') {
      if ([string]::IsNullOrWhiteSpace($env:ANDROID_NDK_HOME) -or
          -not (Test-Path -LiteralPath (Join-Path $env:ANDROID_NDK_HOME 'build/cmake/android.toolchain.cmake'))) {
        throw 'An exact installed Android NDK is required; no source/platform fallback'
      }
      $options += @(
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $env:ANDROID_NDK_HOME 'build/cmake/android.toolchain.cmake')",
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-26'
      )
    } elseif ($Rid -eq 'ios-arm64') {
      if (-not $IsMacOS) { throw "iOS ASN1 requires an Apple SDK host" }
      $options += @('-DCMAKE_SYSTEM_NAME=iOS',
                    '-DCMAKE_OSX_SYSROOT=iphoneos',
                    '-DCMAKE_OSX_ARCHITECTURES=arm64')
    } elseif ($Rid -notin @('linux-x64', 'linux-arm64', 'macos-arm64')) {
      throw "Unsupported exact target RID: $Rid"
    }
  }
  Invoke-Cmake $options "configure $Rid"
  Invoke-Cmake @('--build', $build, '--config', 'Release', '--parallel', '2') "build $Rid"
  if ($RunHostTests) {
    Invoke-CTest @('--test-dir', $build, '-C', 'Release',
                   '--no-tests=error', '--output-on-failure', '--timeout', '120')
  }
  Invoke-Cmake @('--install', $build, '--config', 'Release') "install $Rid"

  $archive = if ($Rid -eq 'windows-x64') { 'salts_asn1.lib' } else { 'libsalts_asn1.a' }
  $expected = @(
    (Join-Path $prefix "lib/$archive"),
    (Join-Path $prefix 'include/salts/asn1.h'),
    (Join-Path $prefix 'include/salts/asn1_value.h'),
    (Join-Path $prefix 'lib/cmake/ASN1/ASN1Config.cmake')
  )
  foreach ($file in $expected) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
      throw "Missing exact ASN1 SDK artifact: $file"
    }
  }

  # Run real installed C11 and C++17 consumers for every runnable host RID.
  if ($RunHostTests) {
    $consumerBuild = Join-Path $builds "$Rid-installed"
    $consumerOptions = @(
      '-S', (Join-Path $source 'tests/installed'), '-B', $consumerBuild,
      '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$prefix"
    )
    if ($Rid -eq 'windows-x64') {
      $consumerOptions += @('-G', 'Visual Studio 18 2026', '-A', 'x64')
    } else {
      $consumerOptions += @('-G', 'Ninja')
    }
    Invoke-Cmake $consumerOptions "installed consumers configure $Rid"
    Invoke-Cmake @('--build', $consumerBuild, '--config', 'Release', '--parallel', '2') "installed consumers build $Rid"
    Invoke-CTest @('--test-dir', $consumerBuild, '-C', 'Release',
                   '--no-tests=error', '--output-on-failure', '--timeout', '120')
  }
  return $prefix
}

$hostRoot = Build-ASN1 $HostRid $true
$targetRoot = if ($HostRid -ceq $TargetRid) {
  $hostRoot
} else {
  Build-ASN1 $TargetRid $false
}
if ([string]::IsNullOrWhiteSpace($targetRoot)) { throw 'Missing target ASN1 SDK' }

$variables = [ordered]@{
  SALTS_ASN1_ROOT = $targetRoot
  SALTS_ASN1_HOST_ROOT = $hostRoot
  SALTS_ASN1_SOURCE_SHA = $cryptoCommit
}
foreach ($entry in $variables.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
  if ($env:GITHUB_ENV) { "$($entry.Key)=$($entry.Value)" >> $env:GITHUB_ENV }
}
Write-Host "PASS exact qigao/crypto/asn1@$($cryptoCommit): $TargetRid -> $targetRoot, host $HostRid -> $hostRoot"
