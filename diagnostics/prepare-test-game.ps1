param([string]$Source = 'D:\Steam\steamapps\common\Alien Shooter',
    [ValidateSet('default', 'client-a', 'client-b')][string]$Instance = 'default')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-session.ps1')
$sessionLock = Enter-TestSessionLock
try {
$sourcePath = (Resolve-Path -LiteralPath $Source).Path
$target = Get-TestGameDirectory $Instance
$target = Assert-TestDirectory $target $PSScriptRoot
Assert-TestGameIdle $target
if ($sourcePath -ieq $target -or $sourcePath.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase) -or $target.StartsWith($sourcePath + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Source overlaps the diagnostic destination.' }
$expectedHash = Get-TestExeHash
$exe = Join-Path $sourcePath 'AlienShooter.exe'
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Unsupported game EXE.' }
$bytes = (Get-ChildItem -LiteralPath $sourcePath -File -Recurse | Measure-Object Length -Sum).Sum
Assert-TestFreeSpace $target $bytes
Assert-TestTree $sourcePath
if (Test-Path -LiteralPath $target) {
    Assert-TestTree $target
    $marker = Join-Path $target 'asmp-diag-test.marker'
    if (!(Test-Path -LiteralPath $marker -PathType Leaf)) { throw 'Existing destination is not a diagnostic copy.' }
    if ((Get-Content -LiteralPath $marker -Raw).Trim() -ne 'ASMP diagnostic copy') { throw 'Invalid test-copy marker.' }
} else {
    New-Item -ItemType Directory -Path $target | Out-Null
    Set-Content -LiteralPath (Join-Path $target 'asmp-diag-test.marker') -Value 'ASMP diagnostic copy' -Encoding ascii
}
Get-ChildItem -LiteralPath $sourcePath -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $target -Recurse -Force }
# Steam development app-ID file allows launching this owned game copy directly.
Set-Content -LiteralPath (Join-Path $target 'steam_appid.txt') -Value '33100' -Encoding ascii
if ((Get-FileHash -LiteralPath (Join-Path $target 'AlienShooter.exe') -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Copied EXE verification failed.' }
Write-Output "Prepared test copy: $target"
} finally { $sessionLock.Dispose() }
