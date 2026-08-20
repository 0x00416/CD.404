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
    'assets/fonts/NotoSansCJKsc-VF.ttf',
    'assets/fonts/OFL-1.1.txt',
    'assets/fonts/README.md',
    'installer/native/CMakeLists.txt',
    'installer/native/main.cpp',
    'installer/native/wizard.cpp',
    'installer/native/wizard.hpp',
    'installer/native/installer.manifest',
    'installer/native/resources.rc.in',
    'tools/test-installer.ps1',
    'apps/cd404/app.manifest',
    'apps/cd404/resources.rc.in',
    'src/core/include/cd404/core/version.hpp'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $relative) -PathType Leaf)) {
        throw "Required release file is missing: $relative"
    }
}

$font = Join-Path $repoRoot 'assets/fonts/NotoSansCJKsc-VF.ttf'
$fontHash = (Get-FileHash -LiteralPath $font -Algorithm SHA256).Hash
if ($fontHash -ne '990C807E79C25662A5A9ECF7F971BAEB2BF2EAB9A559E5ECF15CDFDB8561D21F') {
    throw "Bundled Noto Sans CJK font hash mismatch: $fontHash"
}

$cmake = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
$versionHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'src/core/include/cd404/core/version.hpp') -Raw
$installer = Get-Content -LiteralPath (Join-Path $repoRoot 'installer/native/CMakeLists.txt') -Raw
if ($cmake -notmatch 'VERSION\s+0\.2\.0' -or
    $versionHeader -notmatch '0\.2\.0-public-beta\.1' -or
    $installer -notmatch 'public-beta\.1') {
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
