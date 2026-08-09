[CmdletBinding()]
param(
    [switch]$Sign
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    & make release
    if ($LASTEXITCODE -ne 0) {
        throw 'Release build or tests failed.'
    }
    & (Join-Path $PSScriptRoot 'release-check.ps1')

    $compiler = Get-Command iscc.exe -ErrorAction SilentlyContinue
    $candidatePaths = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    )
    $compilerPath = if ($compiler) {
        $compiler.Source
    } else {
        $candidatePaths | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (-not $compilerPath) {
        throw 'Inno Setup compiler (ISCC.exe) is required to build the installer.'
    }

    & $compilerPath (Join-Path $repoRoot 'installer\CD.404.iss')
    if ($LASTEXITCODE -ne 0) {
        throw 'Inno Setup compilation failed.'
    }
    $package = Join-Path $repoRoot 'out\package\CD.404-0.2.0-public-beta.1-x64-setup.exe'
    if ($Sign) {
        $thumbprint = $env:CD404_SIGNING_THUMBPRINT
        $signTool = Get-Command signtool.exe -ErrorAction SilentlyContinue
        if (-not $thumbprint -or -not $signTool) {
            throw 'Signing requires signtool.exe and CD404_SIGNING_THUMBPRINT.'
        }
        & $signTool.Source sign /sha1 $thumbprint /fd SHA256 /tr 'http://timestamp.digicert.com' /td SHA256 $package
        if ($LASTEXITCODE -ne 0) {
            throw 'Authenticode signing failed.'
        }
        & $signTool.Source verify /pa $package
        if ($LASTEXITCODE -ne 0) {
            throw 'Authenticode verification failed.'
        }
    }

    $hash = Get-FileHash -LiteralPath $package -Algorithm SHA256
    $hashLine = "$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($package))"
    Set-Content -LiteralPath "$package.sha256" -Value $hashLine -Encoding ascii
    Write-Host "[CD.404] Package: $package"
    Write-Host "[CD.404] SHA-256: $($hash.Hash.ToLowerInvariant())"
} finally {
    Pop-Location
}
