# Stops the mod in both clients and then the relay of the managed two-client session.
param(
    [switch]$CloseGames,
    [switch]$Force,
    [ValidateRange(1, 300)][int]$TimeoutSeconds = 15
)
$ErrorActionPreference = 'Stop'
if ($Force -and !$CloseGames) { throw '-Force only applies together with -CloseGames.' }
. (Join-Path $PSScriptRoot 'test-session.ps1')
. (Join-Path $PSScriptRoot 'two-client.ps1')
$orchestrationLock = Enter-TwoClientLock
try {
    if (!(Read-TwoClientManifest)) { Write-Output 'No two-client session is recorded.'; return }
    $unclean = @(Stop-TwoClientSession -TimeoutSeconds $TimeoutSeconds -CloseGames:$CloseGames -Force:$Force)
    if ($unclean.Count) {
        $unclean | ForEach-Object { Write-Warning $_ }
        throw 'Cleanup is incomplete; the manifest keeps the details. Fix the items above and run this command again.'
    }
    if ($CloseGames) { Write-Output 'Mod stopped, relay stopped and game windows closed.' }
    else { Write-Output 'Mod stopped and relay stopped. The game windows remain open; close them yourself or rerun with -CloseGames.' }
} finally { $orchestrationLock.Dispose() }
