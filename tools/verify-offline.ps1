[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot 'windows-common.ps1')
try {
    $manifestPath = Join-Path $ProjectRoot 'offline-manifest.json'
    Assert-File $manifestPath
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $failed = 0
    foreach ($entry in $manifest) {
        $filePath = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $entry.Path))
        if (-not $filePath.StartsWith($ProjectRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Manifest path is outside the package: $($entry.Path)"
        }
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            Write-Warning "Missing: $($entry.Path)"; $failed++; continue
        }
        if ((Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash -ne $entry.SHA256) {
            Write-Warning "Changed or damaged: $($entry.Path)"; $failed++
        }
    }
    if ($failed) { throw "$failed files failed verification. Recopy the package or check intentional source edits." }
    Write-Host "Verified $(@($manifest).Count) files."
} catch { Write-Error $_; exit 1 }
