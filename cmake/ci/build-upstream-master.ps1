param(
  [Parameter(Mandatory=$true)][string]$TargetRid,
  [Parameter(Mandatory=$true)][string]$HostRid
)
$ErrorActionPreference = 'Stop'

# One coherent master snapshot is resolved by the workflow, then checked out
# at exact SHAs for every RID. Never consult a release, tag, NuGet package or
# legacy Salts/SaltsUtils binary while constructing these native dependencies.
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$coreSource = Join-Path $repo 'build/upstream/salts-core'
$saltsSource = Join-Path $repo 'build/upstream/salts'
$paths = @{
  Core = $coreSource
  Salts = $saltsSource
}
$expected = @{
  Core = $env:SALTS_CORE_SOURCE_SHA
  Salts = $env:SALTS_SOURCE_SHA
}
foreach ($name in @('Core','Salts')) {
  if ([string]::IsNullOrWhiteSpace($expected[$name]) -or
      $expected[$name] -cnotmatch '^[0-9a-f]{40}$') {
    throw "Missing exact resolved upstream master SHA for $name"
  }
  if (-not (Test-Path -LiteralPath (Join-Path $paths[$name] 'CMakeLists.txt') -PathType Leaf)) {
    throw "Missing upstream master source for $name"
  }
  $actual = (& git -C $paths[$name] rev-parse HEAD | Out-String).Trim()
  if ($LASTEXITCODE -ne 0 -or $actual -cne $expected[$name]) {
    throw "Upstream $name HEAD mismatch: expected $($expected[$name]); got $actual"
  }
  Write-Host "Verified upstream $name master snapshot: $actual"
}

$toolchain = Join-Path $repo 'build/vcpkg-cache-contract/cmake/QigaoVcpkgToolchain.cmake'
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
  throw "Missing canonical curl-only vcpkg-cache toolchain"
}
if (-not $env:VCPKG_ROOT -or -not (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'))) {
  throw 'Canonical vcpkg toolchain is not installed'
}
if (-not $env:RE2C_ROOT -or
    -not (Test-Path -LiteralPath (Join-Path $env:RE2C_ROOT "bin/$(if ($IsWindows) { 're2c.exe' } else { 're2c' })"))) {
  throw 'Exact host re2c tool and Unicode property database are required'
}
if (-not $env:VCPKG_OVERLAY_PORTS) {
  throw 'Canonical vcpkg-cache overlay ports are required'
}
$roots = Join-Path $repo 'stage/master-source'
$builds = Join-Path $repo 'build/master-source'
New-Item -ItemType Directory -Force -Path $roots,$builds | Out-Null

function Invoke-Cmake([string[]]$Arguments,[string]$Context) {
  & cmake @Arguments | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "Upstream master $Context failed" }
}
function Require([string]$File) {
  if (-not (Test-Path -LiteralPath $File -PathType Leaf)) {
    throw "Missing exact upstream master SDK file: $File"
  }
}
function Platform-Arguments([string]$Rid) {
  $args = @()
  if ($Rid -eq 'windows-x64') {
    if (-not $IsWindows) { throw 'Windows source build requires a real Windows host' }
    $args += @('-G','Visual Studio 18 2026','-A','x64')
  } else {
    $args += @('-G','Ninja')
    if ($Rid -eq 'android-arm64-v8a') {
      $ndk = $env:ANDROID_NDK_HOME
      if (-not $ndk -or -not (Test-Path -LiteralPath (Join-Path $ndk 'build/cmake/android.toolchain.cmake'))) {
        throw 'Native Android cross build requires the installed exact NDK'
      }
      # Vcpkg's arm64-android triplet chainloads the real Android toolchain.
      $args += @('-DCMAKE_SYSTEM_NAME=Android',
                 '-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-26')
    } elseif ($Rid -eq 'ios-arm64') {
      if (-not $IsMacOS) { throw 'iOS source build requires a real macOS host' }
      $args += @('-DCMAKE_SYSTEM_NAME=iOS',
                 '-DCMAKE_OSX_SYSROOT=iphoneos',
                 '-DCMAKE_OSX_ARCHITECTURES=arm64')
    } elseif ($Rid -notin @('linux-x64','linux-arm64','macos-arm64')) {
      throw "Unknown native RID $Rid"
    }
  }
  return ,$args
}
function Build-Upstream([string]$Rid,[string]$Triplet,[string]$HostTriplet) {
  $buildBase = Join-Path $builds $Rid
  $sdkBase = Join-Path $roots $Rid
  $corePrefix = Join-Path $sdkBase 'core'
  $saltsPrefix = Join-Path $sdkBase 'salts'
  $dependencies = Join-Path $buildBase 'vcpkg_installed'
  $dependencyPrefix = Join-Path $dependencies $Triplet
  $coreBuild = Join-Path $buildBase 'core'
  $saltsBuild = Join-Path $buildBase 'salts'
  $generator = Platform-Arguments $Rid
  $common = @(
    '-DCMAKE_BUILD_TYPE=Release',
    '-DBUILD_TESTING=OFF',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    "-DVCPKG_HOST_TRIPLET=$HostTriplet",
    "-DVCPKG_INSTALLED_DIR=$dependencies",
    '-DVCPKG_APPLOCAL_DEPS=OFF',
    '-DCMAKE_POSITION_INDEPENDENT_CODE=ON'
  )
  # Only the canonical source-owned CNet + URI/TinyTest native targets. CNet's
  # vcpkg manifest 'cnet' feature installs its exact c-ares/kcp/GmSSL/simde
  # providers. No local CNet copy or binary-provider fallback.
  $coreArgs = @(
    '-S',$coreSource,'-B',$coreBuild
  ) + $generator + $common + @(
    '-DVCPKG_MANIFEST_MODE=ON',
    '-DVCPKG_MANIFEST_FEATURES=cnet',
    '-DSALTSCORE_BUILD_URI=ON',
    '-DSALTSCORE_BUILD_CNET=ON',
    '-DSALTSCORE_BUILD_TINYTEST=ON',
    '-DSALTSCORE_BUILD_UNICODE=ON',
    "-DCMAKE_INSTALL_PREFIX=$corePrefix"
  )
  Invoke-Cmake $coreArgs "Core configure [$Rid]"
  Invoke-Cmake @('--build',$coreBuild,'--config','Release','--parallel','2') "Core build [$Rid]"
  Invoke-Cmake @('--install',$coreBuild,'--config','Release') "Core install [$Rid]"
  Require (Join-Path $corePrefix 'lib/cmake/SaltsCore/SaltsCoreConfig.cmake')
  Require (Join-Path $corePrefix 'include/cnet/cnet.h')
  Require (Join-Path $corePrefix 'include/tinytest.h')
  Require (Join-Path $corePrefix 'include/salts_unicode.h')

  # Salts master retains only CMeta/ACE/CFlow and MUST import the above
  # Core DSOs. A configure that sees the old Salts 2.3 package is rejected.
  $saltsArgs = @(
    '-S',$saltsSource,'-B',$saltsBuild
  ) + $generator + $common + @(
    '-DVCPKG_MANIFEST_MODE=OFF',
    "-DSaltsCore_DIR=$(Join-Path $corePrefix 'lib/cmake/SaltsCore')",
    "-DCMAKE_PREFIX_PATH=$corePrefix;$dependencyPrefix",
    "-DCMAKE_INSTALL_PREFIX=$saltsPrefix"
  )
  Invoke-Cmake $saltsArgs "Salts CMeta/CFlow configure [$Rid]"
  Invoke-Cmake @('--build',$saltsBuild,'--config','Release','--parallel','2') "Salts CMeta/CFlow build [$Rid]"
  Invoke-Cmake @('--install',$saltsBuild,'--config','Release') "Salts CMeta/CFlow install [$Rid]"
  Require (Join-Path $saltsPrefix 'lib/cmake/Salts/SaltsConfig.cmake')
  return @{ Core=$corePrefix; Salts=$saltsPrefix }
}

if ($env:QIGAO_TARGET_TRIPLET -eq '' -or $env:QIGAO_HOST_TRIPLET -eq '') {
  throw 'Missing target/host vcpkg ABI triplets'
}
$hostTriplet = $env:QIGAO_HOST_TRIPLET
$targetTriplet = $env:QIGAO_TARGET_TRIPLET
$host = Build-Upstream $HostRid $hostTriplet $hostTriplet
$target = if ($HostRid -ceq $TargetRid) {
  $host
} else {
  Build-Upstream $TargetRid $targetTriplet $hostTriplet
}
$variables = [ordered]@{
  SALTS_ROOT = $target.Salts
  SALTS_CORE_ROOT = $target.Core
  SALTS_HOST_ROOT = $host.Salts
  SALTS_CORE_HOST_ROOT = $host.Core
  SALTS_VERSION = '3.0.0-master'
  SALTS_SOURCE_SHA = $expected.Salts
  SALTS_CORE_SOURCE_SHA = $expected.Core
}
foreach ($pair in $variables.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($pair.Key,[string]$pair.Value,'Process')
  if ($env:GITHUB_ENV) { "$($pair.Key)=$($pair.Value)" >> $env:GITHUB_ENV }
}
Write-Host "PASS canonical source-built master CNet/Core/ACE: $TargetRid Core=$($expected.Core) Salts=$($expected.Salts)"
