[CmdletBinding()]
param(
    [string]$Destination,
    [string]$CubeCltRoot,
    [switch]$SourceOnly
)
. (Join-Path $PSScriptRoot 'windows-common.ps1')

function Copy-Tree([string]$From, [string]$To, [string[]]$ExtraArgs = @()) {
    & robocopy.exe $From $To /E /XJ /R:1 /W:1 /NFL /NDL /NJH /NJS /NP @ExtraArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copy failed ($LASTEXITCODE): $From -> $To" }
}

try {
    if (-not $Destination) { $Destination = Join-Path $ProjectRoot 'offline-package/STM32N6' }
    $destinationRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
    # Require a new folder; never merge stale source, tools, or build caches.
    if (Test-Path -LiteralPath $destinationRoot) { throw "Destination already exists. Choose a new empty destination: $destinationRoot" }
    foreach ($sourceDir in @('Inc', 'Src', 'Lib', 'Gcc', 'STM32Cube_FW_N6', 'Model', 'Binary', 'Doc', 'tools', '.vscode', '_htmresc')) {
        $sourcePath = (Join-Path $ProjectRoot $sourceDir).TrimEnd('\') + '\'
        if ($destinationRoot.StartsWith($sourcePath, [StringComparison]::OrdinalIgnoreCase) -or
            $destinationRoot -ieq $sourcePath.TrimEnd('\')) { throw 'Destination cannot be inside a source directory.' }
    }
    $clt = $null
    if (-not $SourceOnly) {
        $clt = Find-CubeClt $CubeCltRoot
        if (-not $clt) { throw 'STM32CubeCLT not found. Supply -CubeCltRoot, or use -SourceOnly for a PC with tools already installed.' }
        if ($destinationRoot.StartsWith($clt.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Destination cannot be inside the tool installation being copied.'
        }
        # Validate executables, dependent DLLs and loader before copying anything.
        $checkedTools = Initialize-Stm32Tools -CubeCltRoot $clt -BuildTools
        Invoke-Checked $checkedTools.Signing @('-h') | Out-Null
        Invoke-Checked $checkedTools.Programmer @('--help') | Out-Null
    }
    New-Item -ItemType Directory -Path $destinationRoot | Out-Null
    foreach ($sourceDir in @('Inc', 'Src', 'Lib', 'Gcc', 'STM32Cube_FW_N6', 'Doc', '_htmresc', '.vscode')) {
        Copy-Tree (Join-Path $ProjectRoot $sourceDir) (Join-Path $destinationRoot $sourceDir)
    }
    Copy-Tree (Join-Path $ProjectRoot 'Model') (Join-Path $destinationRoot 'Model') @('/XD',
        'st_ai_output', 'st_ai_ws', 'my_mdescs', 'my_mpools', '__pycache__')
    New-Item -ItemType Directory -Path (Join-Path $destinationRoot 'Binary') | Out-Null
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'Binary/ai_fsbl.hex') -Destination (Join-Path $destinationRoot 'Binary')
    New-Item -ItemType Directory -Path (Join-Path $destinationRoot 'tools') | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'tools') -File -Filter '*.ps1' |
        Copy-Item -Destination (Join-Path $destinationRoot 'tools')
    foreach ($pattern in @('*.cmake', 'CMakeLists.txt', 'CMakePresets.json', '*.ioc', '*.cmd', 'README.md', 'LICENSE.md')) {
        Get-ChildItem -LiteralPath $ProjectRoot -File -Filter $pattern | Copy-Item -Destination $destinationRoot
    }
    if ($clt) {
        Write-Host 'Copying complete STM32CubeCLT installation (including DLLs, loaders and driver installers)...'
        Copy-Tree $clt (Join-Path $destinationRoot 'tools/STM32CubeCLT')
    }
    # Relative SHA-256 manifest makes USB transfer corruption detectable offline.
    Write-Host 'Writing SHA-256 transfer manifest...'
    $manifest = @(Get-ChildItem -LiteralPath $destinationRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
        [PSCustomObject]@{
            Path = $_.FullName.Substring($destinationRoot.Length + 1).Replace('\', '/')
            SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    })
    $manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $destinationRoot 'offline-manifest.json') -Encoding UTF8
    Write-Host "Offline package ready: $destinationRoot"
    Write-Host 'Copy this entire folder to the offline PC. Start with build-windows.cmd -CheckOnly.'
} catch { Write-Error $_; exit 1 }
