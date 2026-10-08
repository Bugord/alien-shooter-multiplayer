param([string]$Source = 'D:\Steam\steamapps\common\Alien Shooter')
$ErrorActionPreference = 'Stop'
$sourcePath = (Resolve-Path -LiteralPath $Source).Path
$target = Join-Path $PSScriptRoot 'test-game'
$expectedHash = '4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142'
$exe = Join-Path $sourcePath 'AlienShooter.exe'
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Unsupported game EXE.' }
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
