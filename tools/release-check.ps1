[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$required = @(
    'README.md',
    'docs/PRIVACY.md',
    'docs/RELEASE_CHECKLIST.md',
    'docs/HARDWARE_VALIDATION.md',
    'THIRD_PARTY_NOTICES.md',
    'installer/CD.404.iss',
    'apps/cd404/app.manifest',
    'apps/cd404/resources.rc.in',
    'src/core/include/cd404/core/version.hpp'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $relative) -PathType Leaf)) {
        throw "Required release file is missing: $relative"
    }
}

$cmake = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
$versionHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'src/core/include/cd404/core/version.hpp') -Raw
$installer = Get-Content -LiteralPath (Join-Path $repoRoot 'installer/CD.404.iss') -Raw
if ($cmake -notmatch 'VERSION\s+0\.2\.0' -or
    $versionHeader -notmatch '0\.2\.0-public-beta\.1' -or
    $installer -notmatch '0\.2\.0-public-beta\.1') {
    throw 'Release version is not synchronized across CMake, runtime and installer.'
}

$tracked = & git -C $repoRoot ls-files
if ($LASTEXITCODE -ne 0) {
    throw 'git ls-files failed.'
}
$forbidden = $tracked | Where-Object {
    $_ -match '(^|/)(out|build)/' -or
    $_ -match '\.(db|db-wal|db-shm|log|pdb|obj|exe|dll|token)$'
}
if ($forbidden) {
    throw "Forbidden generated/private files are tracked: $($forbidden -join ', ')"
}

$localPaths = & git -C $repoRoot grep -n -I 'C:\Users\Administrator' -- .
if ($LASTEXITCODE -eq 0 -and $localPaths) {
    throw 'A local user path is present in tracked source.'
}
if ($LASTEXITCODE -notin @(0, 1)) {
    throw 'git grep failed.'
}

& git -C $repoRoot diff --check
if ($LASTEXITCODE -ne 0) {
    throw 'git diff --check failed.'
}

Write-Host '[CD.404] Release static checks passed.'
