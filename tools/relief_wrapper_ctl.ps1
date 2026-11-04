# Switch the relief wrapper of the path planning plugin on or off.
#   relief_wrapper_ctl.ps1 -Mode 1       install: vendor lib_path_planning.dll -> lib_path_planning_orig.dll, wrapper in its place
#   relief_wrapper_ctl.ps1 -Mode 0       uninstall: wrapper removed, vendor DLL back under its own name
#   relief_wrapper_ctl.ps1 -Mode status  report only
#   relief_wrapper_ctl.ps1 -Mode core    copy a new relief_fix_core.dll only; works while Studio runs (the wrapper
#                                         loads the core for one relief job at a time, so the file is free in between)
# The wrapper forwards every export to the vendor DLL and only wraps GenJs001ReliefGcodeSync (relief_fix\proxy).
# It is installed only over the exact vendor build it was made for (SHA-256 below); any other build is left alone.
# Nothing is changed while the plugin is loaded (the beta Studio and its algo-server running).
param(
    [Parameter(Mandatory = $true)][string]$Mode,
    [string]$PluginDir = (Join-Path $env:LOCALAPPDATA "Programs\xTool Studio\resources\tools\algo-server\plugins\algo-path-planning-plugin"),
    [string]$Wrapper = (Join-Path $PSScriptRoot "..\bin\lib_path_planning.dll"),
    [string]$CoreSrc = (Join-Path $PSScriptRoot "..\bin\relief_fix_core.dll"),
    # SHA-256 of the vendor lib_path_planning.dll the wrapper was built against (xTool Studio 1.10.2 beta)
    [string])
$ErrorActionPreference = "Stop"
$Log = Join-Path $PSScriptRoot "relief_wrapper_ctl.log"
$Dll = Join-Path $PluginDir "lib_path_planning.dll"
$Orig = Join-Path $PluginDir "lib_path_planning_orig.dll"

function Write-Log([string]$msg) {
    $line = (Get-Date -Format "yyyy-MM-dd HH:mm:ss") + "  " + $msg
    Add-Content -LiteralPath $Log -Value $line -Encoding ascii
    Write-Output $line
}
function Get-Sha([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
function Test-Vendor([string]$path) { (Test-Path -LiteralPath $path) -and ((Get-Sha $path) -eq $VendorSha) }
function Test-Wrapper([string]$path) {
    (Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).Length -lt 1MB) -and
    ((Get-AuthenticodeSignature -LiteralPath $path).Status -eq "NotSigned")
}
function Test-InUse([string]$path) {
    # a DLL mapped by a running process cannot be opened for writing
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    try { $fs = [System.IO.File]::Open($path, "Open", "ReadWrite", "None"); $fs.Close(); return $false } catch { return $true }
}
function Ensure-Switches {
    # relief_wrapper.ini: the wrapper reads it on every relief generation; the estimator window's switches write it.
    # Created once from the launcher variables; an existing file (the user's last choice) is left as it is.
    $ini = Join-Path $PluginDir "relief_wrapper.ini"
    if (Test-Path -LiteralPath $ini) { return }
    $pct = if ($env:XTOOL_RELIEF_KEEP_TOP) { $env:XTOOL_RELIEF_KEEP_TOP } else { "5" }
    $feather = if ($env:XTOOL_STRIP_CLEANING -eq "1") { "0" } else { "1" }
    # feather_passes: how many of the slicer's 8 feather passes to keep (8 = as shipped); keep_feather is for older wrappers
    $passes = if ($feather -eq "0") { "0" } else { "8" }
    Set-Content -LiteralPath $ini -Encoding ascii -Value @(
        "# relief wrapper switches, read on every relief generation (written by relief_estimator.py)",
        "keep_top=$pct", "keep_feather=$feather", "feather_passes=$passes", "transparent_as_color=1")
    Write-Log "switches created: keep_top=$pct keep_feather=$feather feather_passes=$passes"
}
function Sync-Core {
    # relief_fix_core.dll next to the wrapper: replaced by copy + rename, so a relief job never sees half a file
    $core = Join-Path $PluginDir "relief_fix_core.dll"
    if (-not (Test-Path -LiteralPath $CoreSrc)) { Write-Log "core source missing: $CoreSrc"; return }
    if ((Test-Path -LiteralPath $core) -and ((Get-Sha $core) -eq (Get-Sha $CoreSrc))) { return }
    $new = "$core.new"
    Copy-Item -LiteralPath $CoreSrc -Destination $new -Force
    try { Move-Item -LiteralPath $new -Destination $core -Force; Write-Log "core updated ($(Get-Sha $core))" }
    catch { Write-Log "core not updated (in use by a running relief job?): $($_.Exception.Message)" }
}
function Get-State {
    if ((Test-Wrapper $Dll) -and (Test-Vendor $Orig)) { return "wrapper" }
    if ((Test-Vendor $Dll) -and -not (Test-Path -LiteralPath $Orig)) { return "vendor" }
    return "unexpected"
}

$state = Get-State
if ($Mode -eq "status") {
    Write-Log "status: $state ($PluginDir)"
    exit 0
}
if ($Mode -eq "core") {
    if ($state -ne "wrapper") { Write-Log "core not copied: wrapper not installed"; exit 1 }
    Sync-Core
    exit 0
}
if ($Mode -eq "1") {
    if ($state -eq "wrapper") {
        Ensure-Switches
        Sync-Core
        if ((Get-Sha $Dll) -eq (Get-Sha $Wrapper)) { Write-Log "wrapper already installed"; exit 0 }
        if (Test-InUse $Dll) { Write-Log "wrapper installed but older; plugin in use, not refreshed"; exit 0 }
        Copy-Item -LiteralPath $Wrapper -Destination $Dll -Force
        Write-Log "wrapper refreshed"
        exit 0
    }
    if ($state -ne "vendor") {
        Write-Log "not installed: plugin folder is not the vendor build the wrapper was made for (lib_path_planning.dll sha $(if (Test-Path -LiteralPath $Dll) { Get-Sha $Dll } else { 'missing' }), orig present: $(Test-Path -LiteralPath $Orig))"
        exit 1
    }
    if (Test-InUse $Dll) { Write-Log "not installed: plugin in use (beta Studio running)"; exit 1 }
    Rename-Item -LiteralPath $Dll -NewName "lib_path_planning_orig.dll"
    Copy-Item -LiteralPath $Wrapper -Destination $Dll
    if ((Get-State) -ne "wrapper") { Write-Log "install check failed: state $(Get-State)"; exit 1 }
    Ensure-Switches
    Sync-Core
    Write-Log "wrapper installed"
    exit 0
}
if ($Mode -eq "0") {
    if ($state -eq "vendor") { Write-Log "vendor DLL already in place"; exit 0 }
    if ($state -ne "wrapper") { Write-Log "not changed: unexpected plugin folder state"; exit 1 }
    if ((Test-InUse $Dll) -or (Test-InUse $Orig)) { Write-Log "not uninstalled: plugin in use (beta Studio running)"; exit 1 }
    Remove-Item -LiteralPath $Dll
    Rename-Item -LiteralPath $Orig -NewName "lib_path_planning.dll"
    if ((Get-State) -ne "vendor") { Write-Log "uninstall check failed: state $(Get-State)"; exit 1 }
    Write-Log "vendor DLL restored"
    exit 0
}
Write-Log "unknown mode '$Mode'"
exit 1
