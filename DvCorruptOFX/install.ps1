# Installs (or with -Uninstall removes) DvCorrupt.ofx.bundle into the standard OpenFX folder
# that VEGAS Pro scans: C:\Program Files\Common Files\OFX\Plugins. Self-elevates (UAC).
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$src  = Join-Path $PSScriptRoot 'build\DvCorrupt.ofx.bundle'
$root = Join-Path $env:CommonProgramFiles 'OFX\Plugins'
$dst  = Join-Path $root 'DvCorrupt.ofx.bundle'
$log  = Join-Path $PSScriptRoot 'build\install.log'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Uninstall) { $args += '-Uninstall' }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $args -Wait -PassThru
    if (Test-Path $log) { Get-Content $log }
    exit $p.ExitCode
}

try {
    if (Get-Process vegas* -ErrorAction SilentlyContinue) { throw 'Close VEGAS Pro first (the plugin DLL is locked while VEGAS runs).' }
    if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    if ($Uninstall) {
        "Removed $dst" | Set-Content $log
    } else {
        if (-not (Test-Path (Join-Path $src 'Contents\Win64\DvCorrupt.ofx'))) { throw "Build first: $src not found" }
        New-Item -ItemType Directory -Force $root | Out-Null
        Copy-Item -Recurse -Force $src $dst
        "Installed to $dst" | Set-Content $log
        Get-ChildItem -Recurse $dst | ForEach-Object { $_.FullName } | Add-Content $log
    }
    exit 0
} catch {
    "ERROR: $($_.Exception.Message)" | Set-Content $log
    exit 1
}
