[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [ValidateSet('Debug', 'Release', 'All')]
    [string]$Configuration = 'All',

    [ValidateSet('x64', 'Win32', 'ARM64', 'All')]
    [string]$Platform = 'All',

    [switch]$IncludeSolutionCache,

    [switch]$IncludeBinLogs,

    [switch]$FailOnLockedFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-RepoRoot {
    $repoRootCandidate = Split-Path -Parent $PSScriptRoot
    return [System.IO.Path]::GetFullPath($repoRootCandidate)
}

function Get-SelectedValues {
    param(
        [string]$Value,
        [string[]]$AllValues
    )

    if ($Value -eq 'All') {
        return $AllValues
    }

    return @($Value)
}

function Test-IsSafePath {
    param(
        [string]$Path,
        [string]$RepoRoot
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }

    $normalizedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\\')
    $normalizedRoot = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\\')

    if ($normalizedPath.Length -le $normalizedRoot.Length) {
        return $false
    }

    return $normalizedPath.StartsWith($normalizedRoot, [System.StringComparison]::OrdinalIgnoreCase)
}

function Remove-PathSafe {
    param(
        [string]$Path,
        [string]$RepoRoot,
        [switch]$FailOnLockedFiles
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    if (-not (Test-IsSafePath -Path $Path -RepoRoot $RepoRoot)) {
        throw "Refused to delete unsafe path: $Path"
    }

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if ($PSCmdlet.ShouldProcess($fullPath, 'Remove directory recursively')) {
        try {
            Remove-Item -LiteralPath $fullPath -Recurse -Force -ErrorAction Stop
            Write-Host "[clean] removed $fullPath"
        }
        catch {
            if ($FailOnLockedFiles) {
                throw
            }

            Write-Warning "Failed to remove '$fullPath'. It may be locked. Error: $($_.Exception.Message)"
        }
    }
}

function Remove-FileSafe {
    param(
        [string]$Path,
        [string]$RepoRoot,
        [switch]$FailOnLockedFiles
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    if (-not (Test-IsSafePath -Path $Path -RepoRoot $RepoRoot)) {
        throw "Refused to delete unsafe file path: $Path"
    }

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if ($PSCmdlet.ShouldProcess($fullPath, 'Remove file')) {
        try {
            Remove-Item -LiteralPath $fullPath -Force -ErrorAction Stop
            Write-Host "[clean] removed $fullPath"
        }
        catch {
            if ($FailOnLockedFiles) {
                throw
            }

            Write-Warning "Failed to remove '$fullPath'. It may be locked. Error: $($_.Exception.Message)"
        }
    }
}

function Remove-EmptyDirectorySafe {
    param(
        [string]$Path,
        [string]$RepoRoot,
        [switch]$FailOnLockedFiles
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }

    if (-not (Test-IsSafePath -Path $Path -RepoRoot $RepoRoot)) {
        throw "Refused to delete unsafe empty-directory path: $Path"
    }

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $child = Get-ChildItem -LiteralPath $fullPath -Force -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $child) {
        return $false
    }

    if ($PSCmdlet.ShouldProcess($fullPath, 'Remove empty directory')) {
        try {
            Remove-Item -LiteralPath $fullPath -Force -ErrorAction Stop
            Write-Host "[clean] removed empty $fullPath"
            return $true
        }
        catch {
            if ($FailOnLockedFiles) {
                throw
            }

            Write-Warning "Failed to remove empty directory '$fullPath'. Error: $($_.Exception.Message)"
        }
    }

    return $false
}

function Remove-EmptyTreeUpward {
    param(
        [string]$StartPath,
        [string]$StopPath,
        [string]$RepoRoot,
        [switch]$FailOnLockedFiles
    )

    if (-not (Test-Path -LiteralPath $StartPath)) {
        return
    }

    $current = [System.IO.Path]::GetFullPath($StartPath).TrimEnd('\')
    $stop = [System.IO.Path]::GetFullPath($StopPath).TrimEnd('\')

    while ($current.Length -gt $stop.Length) {
        $removed = Remove-EmptyDirectorySafe -Path $current -RepoRoot $RepoRoot -FailOnLockedFiles:$FailOnLockedFiles
        if (-not $removed) {
            break
        }

        $parent = Split-Path -Path $current -Parent
        if ([string]::IsNullOrWhiteSpace($parent)) {
            break
        }

        $current = [System.IO.Path]::GetFullPath($parent).TrimEnd('\')
    }
}

$repoRoot = Resolve-RepoRoot
$configurations = Get-SelectedValues -Value $Configuration -AllValues @('Debug', 'Release')
$platforms = Get-SelectedValues -Value $Platform -AllValues @('x64', 'Win32', 'ARM64')

Write-Host "[clean] repoRoot=$repoRoot"
Write-Host "[clean] configuration=$($configurations -join ',')"
Write-Host "[clean] platform=$($platforms -join ',')"

$directoriesToClean = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$filesToClean = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$platformRootsToPrune = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

$rootBuildBase = @(
    $repoRoot,
    (Join-Path $repoRoot 'Win2DViewer')
)

foreach ($base in $rootBuildBase) {
    foreach ($platformName in $platforms) {
        $platformRoot = Join-Path $base $platformName
        if (Test-Path -LiteralPath $platformRoot) {
            [void]$platformRootsToPrune.Add([System.IO.Path]::GetFullPath($platformRoot))
        }

        foreach ($configurationName in $configurations) {
            $candidate = Join-Path (Join-Path $base $platformName) $configurationName
            if (Test-Path -LiteralPath $candidate) {
                [void]$directoriesToClean.Add([System.IO.Path]::GetFullPath($candidate))
            }
        }
    }
}

$generatedFiles = Join-Path $repoRoot 'Win2DViewer\Generated Files'
if (Test-Path -LiteralPath $generatedFiles) {
    [void]$directoriesToClean.Add([System.IO.Path]::GetFullPath($generatedFiles))
}

if ($IncludeSolutionCache) {
    $solutionCacheCandidates = @(
        (Join-Path $repoRoot '.vs')
    )

    foreach ($candidate in $solutionCacheCandidates) {
        if (Test-Path -LiteralPath $candidate) {
            [void]$directoriesToClean.Add([System.IO.Path]::GetFullPath($candidate))
        }
    }

    $dbCandidates = Get-ChildItem -Path $repoRoot -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\.VC\.(db|opendb)$' -or $_.Extension -eq '.suo' }

    foreach ($item in $dbCandidates) {
        [void]$filesToClean.Add($item.FullName)
    }
}

if ($IncludeBinLogs) {
    $binlogs = Get-ChildItem -Path $repoRoot -Recurse -File -Filter *.binlog -ErrorAction SilentlyContinue
    foreach ($item in $binlogs) {
        [void]$filesToClean.Add($item.FullName)
    }
}

if ($directoriesToClean.Count -eq 0 -and $filesToClean.Count -eq 0) {
    Write-Host '[clean] no matching build artifacts found.'
    exit 0
}

Write-Host "[clean] directories=$($directoriesToClean.Count) files=$($filesToClean.Count)"

foreach ($path in $directoriesToClean) {
    Remove-PathSafe -Path $path -RepoRoot $repoRoot -FailOnLockedFiles:$FailOnLockedFiles
}

foreach ($path in $filesToClean) {
    Remove-FileSafe -Path $path -RepoRoot $repoRoot -FailOnLockedFiles:$FailOnLockedFiles
}

foreach ($platformRoot in $platformRootsToPrune) {
    $platformParent = Split-Path -Path $platformRoot -Parent
    Remove-EmptyTreeUpward -StartPath $platformRoot -StopPath $platformParent -RepoRoot $repoRoot -FailOnLockedFiles:$FailOnLockedFiles
}

Write-Host '[clean] done.'
