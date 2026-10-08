$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$game = Join-Path $PSScriptRoot 'test-game\AlienShooter.exe'
if (!(Test-Path -LiteralPath $game)) { throw 'Run prepare-test-game.ps1 first.' }
if (!(Test-Path -LiteralPath (Join-Path $build 'asmp-steam-diag.dll'))) { throw 'Run build.ps1 first.' }
if (@(Get-Process AlienShooter -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $game }).Count) {
    throw 'Close the existing diagnostic game copy before starting the next session.'
}
if (!(Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Open Steam and sign in before starting the test copy.' }
$stop = Join-Path $build 'asmp-diag.stop'
if (Test-Path -LiteralPath $stop) { Remove-Item -LiteralPath $stop }
& (Join-Path $build 'asmp-diag-launch.exe') $game (Join-Path $build 'asmp-steam-diag.dll')
if ($LASTEXITCODE) { throw "Diagnostic launcher failed ($LASTEXITCODE)." }
Write-Output "Start a level; fire, switch weapons, collect ammo, take damage and heal. Inspect $build\logs\asmp-diag-<PID>.log"
