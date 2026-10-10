# Managed first monster test: ordinary host AI, one remote replica, no waves.
param(
    [switch]$Gallery,
    [switch]$Forms,
    [switch]$PredictionTest,
    [ValidateRange(1, 65535)][int]$Port = 27020,
    [ValidateLength(1, 15)][string]$NameA = 'MonsterHost',
    [ValidateLength(1, 15)][string]$NameB = 'MonsterClient',
    [ValidateSet('', 'A', 'B')][string]$RestartClient = '',
    [ValidateRange(5, 600)][int]$TimeoutSeconds = 90,
    [ValidateRange(0, 120)][int]$KillAfterSeconds = 0,
    [ValidateSet('normal', 'gibs')][string]$DeathVariant = 'normal'
)
$ErrorActionPreference = 'Stop'
if ($KillAfterSeconds -gt 0 -and $KillAfterSeconds -lt 5) { throw 'KillAfterSeconds must be zero or 5..120.' }
if ($Forms -and (!$Gallery -or $KillAfterSeconds)) { throw '-Forms requires -Gallery and no -KillAfterSeconds.' }
if ($PredictionTest -and (!$Gallery -or $Forms -or $KillAfterSeconds)) { throw '-PredictionTest requires -Gallery without other timers.' }
$previousKillDelay = $env:ASMP_DIAG_MONSTER_KILL_AFTER
$previousDeathVariant = $env:ASMP_DIAG_MONSTER_DEATH_VARIANT
$previousForms = $env:ASMP_DIAG_MONSTER_FORMS
$previousPrediction = $env:ASMP_DIAG_MONSTER_PREDICTION_TEST
try {
    $env:ASMP_DIAG_MONSTER_KILL_AFTER = if ($KillAfterSeconds) { [string]$KillAfterSeconds } else { $null }
    $env:ASMP_DIAG_MONSTER_DEATH_VARIANT = $DeathVariant
    $env:ASMP_DIAG_MONSTER_FORMS = if ($Forms) { '1' } else { $null }
    $env:ASMP_DIAG_MONSTER_PREDICTION_TEST = if ($PredictionTest) { '1' } else { $null }
    & (Join-Path $PSScriptRoot 'start-two-client-test.ps1') -MonsterNetwork -Port $Port -NameA $NameA -NameB $NameB `
        -RestartClient $RestartClient -TimeoutSeconds $TimeoutSeconds -MonsterGallery:$Gallery
} finally {
    $env:ASMP_DIAG_MONSTER_KILL_AFTER = $previousKillDelay
    $env:ASMP_DIAG_MONSTER_DEATH_VARIANT = $previousDeathVariant
    $env:ASMP_DIAG_MONSTER_FORMS = $previousForms
    $env:ASMP_DIAG_MONSTER_PREDICTION_TEST = $previousPrediction
}
