[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Debug-LCD')][string]$Preset = 'Debug',
    [string]$CubeCltRoot,
    [string]$ProgrammerRoot,
    [string]$SerialNumber,
    [switch]$All,
    [switch]$DryRun
)
. (Join-Path $PSScriptRoot 'windows-common.ps1')
try {
    $stmTools = Initialize-Stm32Tools -CubeCltRoot $CubeCltRoot -ProgrammerRoot $ProgrammerRoot
    $images = @()
    if ($All) {
        $images += @{ Path = Join-Path $ProjectRoot 'Binary/ai_fsbl.hex'; Address = $null }
        $images += @{ Path = Join-Path $ProjectRoot 'Model/fd_data.hex'; Address = $null }
        $images += @{ Path = Join-Path $ProjectRoot 'Model/faceid_data.hex'; Address = $null }
    }
    $images += @{ Path = Join-Path $ProjectRoot "build/$Preset/Project_sign.bin"; Address = '0x70100000' }
    # Validate every input before the first write. HEX files carry their own addresses.
    foreach ($image in $images) { Assert-File $image.Path }
    Write-Host 'Board must be in DEV mode and connected through ST-LINK USB.'
    foreach ($image in $images) {
        $cliArgs = @('-c', 'port=SWD', 'mode=HOTPLUG')
        if ($SerialNumber) { $cliArgs += "sn=$SerialNumber" }
        $cliArgs += @('-el', $stmTools.Loader, '-w', $image.Path)
        if ($image.Address) { $cliArgs += $image.Address }
        $cliArgs += '-v'
        Write-Host "$($image.Path) $($image.Address)"
        if ($DryRun) {
            Write-Host ($stmTools.Programmer + ' ' + (($cliArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '))
        } else { Invoke-Checked $stmTools.Programmer $cliArgs }
    }
    if ($DryRun) { Write-Host 'Dry run complete; no hardware was accessed.' }
    else { Write-Host 'Programming verified. Select boot-from-flash mode and power cycle the board.' }
} catch { Write-Error $_; exit 1 }
