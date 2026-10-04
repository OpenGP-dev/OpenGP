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
# then C:\Program Files\LLVM, then Visual Studio's bundled Clang.

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (or Build Tools) with the C++ workload is required.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the MSVC x64 tools was found.' }

# VsDevCmd calls vswhere by name, so make it findable.
$env:PATH = (Split-Path $vswhere) + ';' + $env:PATH
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$candidates = @($env:OPENGP_LLVM, (Join-Path $env:LOCALAPPDATA 'Programs\LLVM'), 'C:\Program Files\LLVM',
                (Join-Path $vs 'VC\Tools\Llvm\x64')) | Where-Object { $_ }
$llvm = $candidates | Where-Object { Test-Path (Join-Path $_ 'bin\clang-cl.exe') } | Select-Object -First 1
if (-not $llvm) { throw 'clang-cl not found. Install LLVM (see docs/building.md) or set OPENGP_LLVM.' }
$env:PATH = (Join-Path $llvm 'bin') + ';' + $env:PATH

Write-Host "OpenGP dev shell: $(Split-Path $vs -Leaf) + $((& clang-cl --version | Select-Object -First 1))"
