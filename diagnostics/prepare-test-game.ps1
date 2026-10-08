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
$expectedHash = '4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142'
$exe = Join-Path $sourcePath 'AlienShooter.exe'
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Unsupported game EXE.' }
$bytes = (Get-ChildItem -LiteralPath $sourcePath -File -Recurse | Measure-Object Length -Sum).Sum
$drive = Get-PSDrive -Name ([IO.Path]::GetPathRoot($target).Substring(0, 1))
if ($null -ne $drive.Free -and $drive.Free -lt $bytes + 64MB) { throw 'Not enough free space for a separate test copy.' }
if (Test-Path -LiteralPath $target) {
    if (!(Test-Path -LiteralPath (Join-Path $target 'asmp-diag-test.marker'))) { throw 'Existing destination is not a diagnostic copy.' }
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
