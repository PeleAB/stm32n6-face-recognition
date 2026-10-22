[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Debug-LCD')][string]$Preset = 'Debug',
    [string]$CubeCltRoot,
    [string]$ProgrammerRoot,
    [ValidateRange(1, 128)][int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount),
    [switch]$CheckOnly,
    [switch]$ConfigureOnly,
    [switch]$Fresh
)
. (Join-Path $PSScriptRoot 'windows-common.ps1')
try {
    $stmTools = Initialize-Stm32Tools -CubeCltRoot $CubeCltRoot -ProgrammerRoot $ProgrammerRoot -BuildTools
    foreach ($required in @('Lib/n6-platform/CMakeLists.txt', 'Lib/n6-platform/Src/sysobj/src/sysobj_params.c',
        'Lib/n6-platform/Src/sysobj/src/sysobj_timer.c', 'Lib/AI_Runtime/Inc/stai.h',
        'Lib/AI_Runtime/Npu/ll_aton/stai_aton_common.h', 'Lib/AI_Runtime/Lib/GCC/ARMCortexM55/NetworkRuntime1020_CM55_GCC.a',
        'Model/fd.c', 'Model/faceid.c', 'Model/fd_data.hex', 'Model/faceid_data.hex', 'Binary/ai_fsbl.hex')) {
        Assert-File (Join-Path $ProjectRoot $required)
    }
    Write-Host "Project: $ProjectRoot"
    Write-Host "Compiler: $($stmTools.Compiler)"
    Write-Host "Signing tool: $($stmTools.Signing)"
    Invoke-Checked $stmTools.CMake @('--version')
    Invoke-Checked $stmTools.Ninja @('--version')
    Invoke-Checked $stmTools.Compiler @('--version')
    # Load DLL dependencies now, so missing runtimes fail before a lengthy build.
    Invoke-Checked $stmTools.Programmer @('--help') | Out-Null
    Invoke-Checked $stmTools.Signing @('-h') | Out-Null
    if ($CheckOnly) { Write-Host 'Offline build prerequisites found.'; exit 0 }

    $buildDir = Join-Path $ProjectRoot "build/$Preset"
    $cacheFile = Join-Path $buildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cacheFile) {
        $cache = Get-Content -LiteralPath $cacheFile
        $cachedHome = $cache | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' } | Select-Object -First 1
        $expectedHome = $ProjectRoot.Replace('\', '/')
        if ($cachedHome -and $cachedHome.Substring($cachedHome.IndexOf('=') + 1).TrimEnd('/') -ine $expectedHome.TrimEnd('/')) {
            $Fresh = $true
            Write-Host 'Project was relocated; refreshing the CMake cache.'
        }
    }
    # Explicit paths prevent a different toolchain on PATH or in an old cache being used.
    $configure = @('-S', $ProjectRoot, '-B', $buildDir, '--preset', $Preset,
        "-DCMAKE_MAKE_PROGRAM=$($stmTools.Ninja)", "-DSTM32_SIGNING_TOOL=$($stmTools.Signing)",
        "-DN6_PLATFORM_DIR=$(Join-Path $ProjectRoot 'Lib/n6-platform')")
    if ($Fresh) { $configure += '--fresh' } # Supported by the bundled CMake 3.28.
    Push-Location $ProjectRoot
    try {
        Invoke-Checked $stmTools.CMake $configure
        if (-not $ConfigureOnly) { Invoke-Checked $stmTools.CMake @('--build', $buildDir, '--parallel', "$Jobs") }
    } finally { Pop-Location }
    if ($ConfigureOnly) { Write-Host "Configured: $buildDir"; exit 0 }
    Assert-File (Join-Path $buildDir 'Project_sign.bin')
    Write-Host "Ready to flash: $buildDir/Project_sign.bin"
} catch { Write-Error $_; exit 1 }
