param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$pythonPath = (Get-Command $Python -ErrorAction Stop).Source
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install the Visual Studio C++ Build Tools first.' }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'The Visual Studio C++ compiler was not found.' }
    Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
Push-Location -LiteralPath $repoRoot
try {
    & $pythonPath -X utf8 -m unittest discover -s scripts/tests -p test_spotpear_sd_reader.py -v
    if ($LASTEXITCODE -ne 0) { throw 'SD reader tests failed.' }
} finally { Pop-Location }
