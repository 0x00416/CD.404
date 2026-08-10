[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Package,

    [Parameter(Mandatory)]
    [string]$ExpectedApplication,

    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'CD404-Installer-Smoke\CD.404')
)

$ErrorActionPreference = 'Stop'
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$expectedApplicationPath = (Resolve-Path -LiteralPath $ExpectedApplication).Path
$installDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$migratedDirectory = "$installDirectory-Moved"
$installedApplication = Join-Path $installDirectory 'CD.404.exe'
$uninstaller = Join-Path $installDirectory 'Uninstall.exe'
$startMenuShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\CD.404.lnk'
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'CD.404.lnk'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\CD.404'
$userDataDirectory = Join-Path $env:LOCALAPPDATA 'CD.404'
$userDataExisted = Test-Path -LiteralPath $userDataDirectory
$temporaryDirectory = Join-Path $env:LOCALAPPDATA 'Temp'
$helperCountBefore = @(
    Get-ChildItem -LiteralPath $temporaryDirectory -Filter 'CD404-Uninstall-*.exe' -File -ErrorAction SilentlyContinue
).Count

$existingArtifacts = @(
    $startMenuShortcut,
    $desktopShortcut,
    $uninstallKey
) | Where-Object { Test-Path -LiteralPath $_ }
foreach ($directory in @($installDirectory, $migratedDirectory)) {
    if ((Test-Path -LiteralPath $directory) -and
        (Get-ChildItem -LiteralPath $directory -Force | Select-Object -First 1)) {
        $existingArtifacts += $directory
    }
}
if ($existingArtifacts) {
    throw "Refusing to overwrite an existing installation: $($existingArtifacts -join ', ')"
}

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
$sentinel = Join-Path $installDirectory 'user-owned-file.txt'
Set-Content -LiteralPath $sentinel -Value 'installer safety sentinel' -Encoding ascii

$installArguments = @('/silent', '/install-dir', "`"$installDirectory`"")
$install = Start-Process -FilePath $packagePath -ArgumentList $installArguments -Wait -PassThru
if ($install.ExitCode -ne 0) {
    throw "Silent installation failed with exit code $($install.ExitCode)."
}
foreach ($required in @(
    $installedApplication,
    $uninstaller,
    (Join-Path $installDirectory 'docs\PRIVACY.md'),
    (Join-Path $installDirectory 'THIRD_PARTY_NOTICES.md'),
    (Join-Path $installDirectory '.cd404-install'),
    $startMenuShortcut,
    $uninstallKey
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Installed artifact is missing: $required"
    }
}
if (Test-Path -LiteralPath $desktopShortcut) {
    throw 'Silent installation unexpectedly created a desktop shortcut.'
}
$expectedHash = (Get-FileHash -LiteralPath $expectedApplicationPath -Algorithm SHA256).Hash
$installedHash = (Get-FileHash -LiteralPath $installedApplication -Algorithm SHA256).Hash
if ($expectedHash -ne $installedHash) {
    throw 'The installed application does not match the Release application payload.'
}


$migrationArguments = @('/silent', '/install-dir', "`"$migratedDirectory`"")
$migration = Start-Process -FilePath $packagePath -ArgumentList $migrationArguments -Wait -PassThru
if ($migration.ExitCode -ne 0) {
    throw "Installation-path migration failed with exit code $($migration.ExitCode)."
}
if (-not (Test-Path -LiteralPath $sentinel) -or
    (Test-Path -LiteralPath $installedApplication)) {
    throw 'Path migration did not preserve an unrelated file or remove the old application.'
}
$installDirectory = $migratedDirectory
$installedApplication = Join-Path $installDirectory 'CD.404.exe'
$uninstaller = Join-Path $installDirectory 'Uninstall.exe'
$registeredLocation = (Get-ItemProperty -LiteralPath $uninstallKey).InstallLocation
if ([IO.Path]::GetFullPath($registeredLocation) -ne $installDirectory) {
    throw 'Windows uninstall registration did not follow the migrated install location.'
}

$runningApplication = Start-Process -FilePath $installedApplication -PassThru
for ($attempt = 0; $attempt -lt 50 -and $runningApplication.MainWindowHandle -eq 0; $attempt++) {
    Start-Sleep -Milliseconds 100
    $runningApplication.Refresh()
}
if ($runningApplication.MainWindowHandle -eq 0) {
    Stop-Process -Id $runningApplication.Id -Force -ErrorAction SilentlyContinue
    throw 'The installed application did not create its main window for close testing.'
}

$deleteLock = [IO.File]::Open(
    $installedApplication,
    [IO.FileMode]::Open,
    [IO.FileAccess]::Read,
    [IO.FileShare]::Read)
$remove = Start-Process -FilePath $uninstaller -ArgumentList '/uninstall /silent' -PassThru
$remove.WaitForExit()
Start-Sleep -Milliseconds 800
$deleteLock.Dispose()
if ($remove.ExitCode -ne 0) {
    throw "Silent uninstall launch failed with exit code $($remove.ExitCode)."
}
if (-not $runningApplication.HasExited) {
    throw 'Uninstall did not close the running CD.404 application.'
}
$deadline = [DateTime]::UtcNow.AddSeconds(15)
while ((Test-Path -LiteralPath $installDirectory) -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
}
$helperDeadline = [DateTime]::UtcNow.AddSeconds(5)
do {
    $helperCountAfter = @(
        Get-ChildItem -LiteralPath $temporaryDirectory -Filter 'CD404-Uninstall-*.exe' -File -ErrorAction SilentlyContinue
    ).Count
    if ($helperCountAfter -le $helperCountBefore) {
        break
    }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $helperDeadline)
if ($helperCountAfter -gt $helperCountBefore) {
    throw 'Uninstall left a temporary cleanup executable behind.'
}
foreach ($removed in @($installDirectory, $startMenuShortcut, $desktopShortcut, $uninstallKey)) {
    if (Test-Path -LiteralPath $removed) {
        throw "Uninstall left an artifact behind: $removed"
    }
}
if ($userDataExisted -and -not (Test-Path -LiteralPath $userDataDirectory)) {
    throw 'Uninstall removed the pre-existing CD.404 user data directory.'
}

Remove-Item -LiteralPath $sentinel -Force
Remove-Item -LiteralPath ([IO.Path]::GetDirectoryName($sentinel)) -Force

Write-Host '[CD.404] Native installer install/uninstall smoke test passed.'
