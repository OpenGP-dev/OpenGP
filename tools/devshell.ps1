# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sets up the current PowerShell session for building OpenGP on Windows with
# clang-cl: enters the Visual Studio x64 developer environment (MSVC headers
# and libraries, Windows SDK) and puts LLVM first on PATH.
#
# Usage (dot-source it so the environment stays in your session):
#   . .\tools\devshell.ps1
#   cmake --preset clang-cl
#   cmake --build --preset clang-cl
#   ctest --preset clang-cl
#
# LLVM is looked up in $env:OPENGP_LLVM, then %LOCALAPPDATA%\Programs\LLVM,
# then C:\Program Files\LLVM, then the Clang of a Visual Studio installation
# that has the "C++ Clang tools for Windows" component. Without a standalone
# LLVM, that installation is the one whose environment is used, even when a
# newer Visual Studio without Clang is installed too. Clang 16 or later is
# required.

# The script is dot-sourced, so it runs in the caller's scope: keep the
# caller's error preference.
$devshellPreviousEap = $ErrorActionPreference
$ErrorActionPreference = 'Stop'
try {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { throw 'Visual Studio (or Build Tools) with the C++ workload is required.' }

  $standalone = @($env:OPENGP_LLVM, (Join-Path $env:LOCALAPPDATA 'Programs\LLVM'), 'C:\Program Files\LLVM') |
    Where-Object { $_ -and (Test-Path (Join-Path $_ 'bin\clang-cl.exe')) } | Select-Object -First 1

  $msvc = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
  $clang = 'Microsoft.VisualStudio.Component.VC.Llvm.Clang'
  if ($standalone) {
    $vs = & $vswhere -latest -products * -requires $msvc -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the MSVC x64 tools was found.' }
    $llvm = $standalone
  } else {
    $vs = & $vswhere -latest -products * -requires $msvc $clang -property installationPath
    if (-not $vs) {
      throw ('clang-cl not found. Install LLVM (see docs/building.md), set OPENGP_LLVM, or add ' +
             '"C++ Clang tools for Windows" to a Visual Studio installation that has the MSVC x64 tools.')
    }
    $llvm = Join-Path $vs 'VC\Tools\Llvm\x64'
  }

  # VsDevCmd calls vswhere by name, so make it findable.
  $installer = Split-Path $vswhere
  if (($env:PATH -split ';') -notcontains $installer) { $env:PATH = $installer + ';' + $env:PATH }
  Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

  $llvmBin = Join-Path $llvm 'bin'
  $env:PATH = (@($llvmBin) + (($env:PATH -split ';') | Where-Object { $_ -and $_ -ne $llvmBin })) -join ';'

  $version = (& clang-cl --version | Select-Object -First 1)
  if ($version -match 'version (\d+)\.' -and [int]$Matches[1] -lt 16) {
    throw "$version is too old: OpenGP needs Clang 16 or later."
  }
  Write-Host "OpenGP dev shell: $(Split-Path $vs -Leaf) + $version"
} finally {
  $ErrorActionPreference = $devshellPreviousEap
  Remove-Variable devshellPreviousEap -ErrorAction SilentlyContinue
}
