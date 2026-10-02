# Configures the Apex CMake build.
#
# Usage:
#   powershell ./scripts/configure.ps1            # Debug + Release capable
#   powershell ./scripts/configure.ps1 -NoTests   # skip test targets
#
# Requires:
#   - $env:VCPKG_ROOT pointing at a bootstrapped vcpkg checkout, OR vcpkg
#     installed at $HOME/vcpkg (the default location used by this project).
#   - A Visual Studio installation with the C++ workload (any recent year).
#     The newest installed toolchain is selected automatically via vswhere.
#
# The Visual Studio generator is used on purpose: unlike Ninja it does not
# require a Developer Prompt environment, and one configure covers both
# Debug and Release. See README.md for the equivalent manual commands.

param([switch]$NoTests)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot

# cmake/ctest may not be on PATH in a fresh shell (winget updates PATH only
# for new logons), so resolve them explicitly before doing anything else.
$CmakeBin = "C:\Program Files\CMake\bin"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  if (-not (Test-Path (Join-Path $CmakeBin "cmake.exe"))) {
    Write-Error "cmake not found. Install it with: winget install -e --id Kitware.CMake"
  }
  $env:PATH = "$CmakeBin;$env:PATH"
}

if (-not $env:VCPKG_ROOT -or -not (Test-Path $env:VCPKG_ROOT)) {
  $DefaultVcpkg = Join-Path $HOME "vcpkg"
  if (Test-Path (Join-Path $DefaultVcpkg "scripts/buildsystems/vcpkg.cmake")) {
    $env:VCPKG_ROOT = $DefaultVcpkg
  } else {
    Write-Error ("VCPKG_ROOT is not set and no vcpkg was found at $DefaultVcpkg. " +
      "Clone https://github.com/microsoft/vcpkg and run bootstrap-vcpkg.bat, " +
      "then set `$env:VCPKG_ROOT to that directory.")
  }
}

$Toolchain = Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"
Write-Host "vcpkg: $env:VCPKG_ROOT"

# Pick the newest installed Visual Studio generator by probing for MSBuild.
# (vswhere reports no instances for this machine's BuildTools layout, so we
# detect the toolchain directories directly. Newest first.)
$Generator = $null
foreach ($cand in @(
  @{ Path = "18\BuildTools"; Name = "Visual Studio 18 2026" },
  @{ Path = "2022\BuildTools"; Name = "Visual Studio 17 2022" },
  @{ Path = "2019\BuildTools"; Name = "Visual Studio 16 2019" })) {
  $msbuild = Join-Path "${Env:ProgramFiles(x86)}\Microsoft Visual Studio" `
    (Join-Path $cand.Path "MSBuild\Current\Bin\MSBuild.exe")
  if (Test-Path $msbuild) {
    $Generator = $cand.Name
    break
  }
}
if (-not $Generator) {
  Write-Error ("No Visual Studio BuildTools installation found under " +
    "'${Env:ProgramFiles(x86)}\Microsoft Visual Studio'. Install the C++ workload first.")
}
Write-Host "Generator: $Generator"

$BuildType = @()
if ($NoTests) { $BuildType += "-DAPEX_BUILD_TESTS=OFF" }

# vcpkg's installed tree must live in a path WITHOUT spaces or shell-special
# characters (notably this repo's own directory name). Meson/pkg-config based
# ports (e.g. libpq) emit backslash-escaped include paths that MSVC's cl.exe
# cannot parse, so building them with an installed tree under this repo fails.
# Redirecting VCPKG_INSTALLED_DIR keeps the manifest as the single source of
# truth while giving those build systems clean paths. The directory is a pure
# local build artifact (outside the repo, git-irrelevant).
$InstalledDir = Join-Path ([System.Environment]::GetFolderPath("LocalApplicationData")) `
  "apex\vcpkg-installed"
$BuildType += "-DVCPKG_INSTALLED_DIR=$InstalledDir"
Write-Host "vcpkg installed dir: $InstalledDir"

& cmake -S $Root -B (Join-Path $Root "build") -G $Generator -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" @BuildType
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Configured. Next: powershell ./scripts/build.ps1 -Config Debug"
