$ErrorActionPreference = 'Stop'

foreach ($name in @('GITHUB_WORKSPACE', 'QIGAO_SDK_RID', 'QIGAO_TARGET_TRIPLET',
                    'QIGAO_HOST_TRIPLET', 'SALTS_ROOT', 'SALTS_UTILS_ROOT',
                    'VCPKG_ROOT', 'VCPKG_CACHE_REPOSITORY_ROOT', 'VCPKG_BINARY_SOURCES')) {
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
# The formal consumer has its own empty manifest/build tree. Only the shared
# external toolchain and installed packages are inputs; no source-tree targets.
$env:SALTSNET_ROOT = $prefix
$triplet = $env:QIGAO_TARGET_TRIPLET
$dependencies = Join-Path $repositoryRoot "vcpkg_installed/$triplet"
$sourceDir = Join-Path $repositoryRoot 'tests/installed_sdk'
# The optional resolved binary cache is only a runtime search location when
# it actually exists; no project dependency shall be rebuilt as a fallback.

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
$preset = if ($IsWindows) { 'win-release-user' } elseif ($IsMacOS) { 'macos-release-user' } else { 'linux-release-user' }
Push-Location $sourceDir
try {
  & cmake --preset $preset
  if ($LASTEXITCODE -ne 0) { throw "Installed SDK C11/C++17 configure failed" }
  & cmake --build --preset $preset --parallel 2
  if ($LASTEXITCODE -ne 0) { throw "Installed SDK C11/C++17 build failed" }
  & ctest --preset $preset
  if ($LASTEXITCODE -ne 0) { throw "Installed SDK C11/C++17 runtime failed" }
} finally {
  Pop-Location
}
