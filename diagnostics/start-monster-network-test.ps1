# Managed first monster test: ordinary host AI, one remote replica, no waves.
param(
    [ValidateRange(1, 65535)][int]$Port = 27020,
    [ValidateLength(1, 15)][string]$NameA = 'MonsterHost',
    [ValidateLength(1, 15)][string]$NameB = 'MonsterClient',
    [ValidateSet('', 'A', 'B')][string]$RestartClient = '',
    [ValidateRange(5, 600)][int]$TimeoutSeconds = 90,
    [ValidateRange(0, 120)][int]$KillAfterSeconds = 0
)
$ErrorActionPreference = 'Stop'
if ($KillAfterSeconds -gt 0 -and $KillAfterSeconds -lt 5) { throw 'KillAfterSeconds must be zero or 5..120.' }
$previousKillDelay = $env:ASMP_DIAG_MONSTER_KILL_AFTER
try {
    $env:ASMP_DIAG_MONSTER_KILL_AFTER = if ($KillAfterSeconds) { [string]$KillAfterSeconds } else { $null }
    & (Join-Path $PSScriptRoot 'start-two-client-test.ps1') -MonsterNetwork -Port $Port -NameA $NameA -NameB $NameB `
        -RestartClient $RestartClient -TimeoutSeconds $TimeoutSeconds
} finally { $env:ASMP_DIAG_MONSTER_KILL_AFTER = $previousKillDelay }
