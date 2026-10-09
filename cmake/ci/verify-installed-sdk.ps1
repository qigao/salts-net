$ErrorActionPreference = 'Stop'

foreach ($name in @('GITHUB_WORKSPACE', 'QIGAO_SDK_RID', 'QIGAO_TARGET_TRIPLET',
                    'QIGAO_HOST_TRIPLET', 'SALTS_ROOT', 'SALTS_UTILS_ROOT',
                    'QIGAO_VCPKG_TOOLCHAIN_FILE')) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "Installed SDK verification requires $name"
  }
}
$repositoryRoot = [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
$prefix = Join-Path $repositoryRoot "stage/sdk/$env:QIGAO_SDK_RID"
$config = Join-Path $prefix 'lib/cmake/SaltsNet/SaltsNetConfig.cmake'
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
  throw "Missing installed SaltsNet config: $config"
}
$toolchain = $env:QIGAO_VCPKG_TOOLCHAIN_FILE
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
  throw "Missing shared vcpkg toolchain: $toolchain"
}
$env:SALTSNET_ROOT = $prefix
$triplet = $env:QIGAO_TARGET_TRIPLET
$dependencies = Join-Path $repositoryRoot "vcpkg_installed/$triplet"
$binaryDir = Join-Path $repositoryRoot "build/ci-installed/$env:QIGAO_SDK_RID"
$sourceDir = Join-Path $repositoryRoot 'tests/installed_sdk'
if (-not (Test-Path -LiteralPath $dependencies -PathType Container)) {
  throw "Missing native dependency tree: $dependencies"
}

# The package import is from the fresh install prefix. Runtime search never
# relies on build/ci-sdk objects or a sibling SaltsNet source-tree target.
$separator = [string][IO.Path]::PathSeparator
$runtimeDirs = @(
  (Join-Path $prefix 'bin'),
  (Join-Path $prefix 'lib'),
  (Join-Path $env:SALTS_ROOT 'bin'),
  (Join-Path $env:SALTS_ROOT 'lib'),
  (Join-Path $env:SALTS_UTILS_ROOT 'bin'),
  (Join-Path $env:SALTS_UTILS_ROOT 'lib'),
  (Join-Path $dependencies 'bin'),
  (Join-Path $dependencies 'lib')
)
$runtimePrefix = ($runtimeDirs | Where-Object { Test-Path -LiteralPath $_ -PathType Container }) -join $separator
$env:PATH = "$runtimePrefix$separator$env:PATH"
if (-not $IsWindows) {
  $env:LD_LIBRARY_PATH = "$runtimePrefix$separator$env:LD_LIBRARY_PATH"
  $env:DYLD_LIBRARY_PATH = "$runtimePrefix$separator$env:DYLD_LIBRARY_PATH"
}
$cmakeArgs = @(
  '-S', $sourceDir, '-B', $binaryDir, '-G', 'Ninja',
  '-DCMAKE_BUILD_TYPE=Release',
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
  "-DVCPKG_TARGET_TRIPLET=$triplet",
  "-DVCPKG_HOST_TRIPLET=$env:QIGAO_HOST_TRIPLET",
  "-DVCPKG_INSTALLED_DIR=$(Join-Path $repositoryRoot 'vcpkg_installed')",
  '-DVCPKG_MANIFEST_MODE=OFF',
  '-DVCPKG_APPLOCAL_DEPS=OFF',
  "-DCMAKE_PREFIX_PATH=$dependencies"
)
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "Installed SDK C11/C++17 configure failed" }
& cmake --build $binaryDir --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Installed SDK C11/C++17 build failed" }
& ctest --test-dir $binaryDir --no-tests=error --output-on-failure --timeout 90
if ($LASTEXITCODE -ne 0) { throw "Installed SDK C11/C++17 runtime failed" }
