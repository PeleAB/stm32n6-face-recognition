# Shared by the offline Windows scripts; compatible with Windows PowerShell 5.1.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Assert-File([string]$FilePath) {
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "Required file missing: $FilePath. Copy the complete project/tool installation, including DLLs and model files."
    }
}

function Find-CubeClt([string]$RequestedRoot) {
    if ($RequestedRoot) {
        $resolved = (Resolve-Path -LiteralPath $RequestedRoot).Path
        Assert-File (Join-Path $resolved 'GNU-tools-for-STM32/bin/arm-none-eabi-gcc.exe')
        return $resolved
    }
    $portableRoot = Join-Path $ProjectRoot 'tools/STM32CubeCLT'
    if (Test-Path -LiteralPath $portableRoot) { return Find-CubeClt $portableRoot }
    if ($env:STM32_CUBE_CLT) { return Find-CubeClt $env:STM32_CUBE_CLT }
    # Discover installed versions without requiring a particular version or PATH.
    $installed = @(Get-ChildItem -Path 'C:/ST/STM32CubeCLT*' -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    foreach ($candidate in $installed) {
        if (Test-Path -LiteralPath (Join-Path $candidate.FullName 'GNU-tools-for-STM32/bin/arm-none-eabi-gcc.exe')) {
            return $candidate.FullName
        }
    }
    return $null
}

function Find-Tool([string]$Name, [string]$PreferredPath) {
    if ($PreferredPath) {
        Assert-File $PreferredPath
        return $PreferredPath
    }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    throw "Cannot find $Name. Supply -CubeCltRoot, set STM32_CUBE_CLT, or copy the full CLT to tools/STM32CubeCLT."
}

function Initialize-Stm32Tools([string]$CubeCltRoot, [string]$ProgrammerRoot, [switch]$BuildTools) {
    $clt = Find-CubeClt $CubeCltRoot
    $programmerBin = $null
    if (-not $ProgrammerRoot) { $ProgrammerRoot = $env:STM32_PROGRAMMER_ROOT }
    if ($ProgrammerRoot) {
        $ProgrammerRoot = (Resolve-Path -LiteralPath $ProgrammerRoot).Path
        if (Test-Path -LiteralPath (Join-Path $ProgrammerRoot 'bin/STM32_Programmer_CLI.exe')) {
            $programmerBin = Join-Path $ProgrammerRoot 'bin'
        } else { $programmerBin = $ProgrammerRoot }
    } elseif ($clt) { $programmerBin = Join-Path $clt 'STM32CubeProgrammer/bin' }

    $toolPaths = @()
    if ($clt) {
        $toolPaths += Join-Path $clt 'GNU-tools-for-STM32/bin'
        $toolPaths += Join-Path $clt 'CMake/bin'
        $toolPaths += Join-Path $clt 'Ninja/bin'
    }
    if ($programmerBin) { $toolPaths += $programmerBin }
    # Only this process and its children see these additions.
    $env:PATH = ($toolPaths + @($env:PATH)) -join ';'
    $programmer = Find-Tool 'STM32_Programmer_CLI.exe' $(if ($programmerBin) { Join-Path $programmerBin 'STM32_Programmer_CLI.exe' })
    $programmerBin = Split-Path -Parent $programmer
    $result = @{
        CubeCltRoot = $clt
        Programmer = $programmer
        Loader = Join-Path $programmerBin 'ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr'
    }
    Assert-File $result.Loader
    if ($BuildTools) {
        $result.CMake = Find-Tool 'cmake.exe' $(if ($clt) { Join-Path $clt 'CMake/bin/cmake.exe' })
        $result.Ninja = Find-Tool 'ninja.exe' $(if ($clt) { Join-Path $clt 'Ninja/bin/ninja.exe' })
        $result.Compiler = Find-Tool 'arm-none-eabi-gcc.exe' $(if ($clt) { Join-Path $clt 'GNU-tools-for-STM32/bin/arm-none-eabi-gcc.exe' })
        $result.Signing = Find-Tool 'STM32_SigningTool_CLI.exe' (Join-Path $programmerBin 'STM32_SigningTool_CLI.exe')
    }
    return $result
}

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed (exit $LASTEXITCODE)." }
}
