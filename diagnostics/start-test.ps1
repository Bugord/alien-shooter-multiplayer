param(
    [switch]$Fullscreen,
    [switch]$DummyActor,
    [switch]$MonsterPuppet,
    [switch]$MonsterFlying,
    [switch]$MonsterCombat,
    [switch]$MonsterDeath,
    [switch]$MonsterReload,
    [ValidateSet('', 'host', 'client')][string]$MonsterNetworkRole = '',
    [switch]$Multiplayer,
    [ValidateRange(640, 1920)][int]$Width = 800,
    [ValidateRange(480, 1080)][int]$Height = 600,
    [string]$ServerAddress = '',
    [ValidateRange(1, 65535)][int]$Port = 27020,
    [ValidateLength(1, 15)][string]$Name = 'SteamTester',
    [ValidateSet('default', 'client-a', 'client-b')][string]$Instance = 'default',
    [string]$RuntimeDirectory = '',
    [string]$LaunchResultPath = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-session.ps1')
$sessionLock = Enter-TestSessionLock
$restoreMonsterOnFailure = $false
try {
$build = Join-Path $PSScriptRoot 'build'
$gameDirectory = Assert-TestDirectory (Get-TestGameDirectory $Instance) $PSScriptRoot
$game = Join-Path $gameDirectory 'AlienShooter.exe'
$runtime = $build
if ($RuntimeDirectory) {
    if ($Instance -eq 'default') { throw 'RuntimeDirectory is reserved for pair instances.' }
    $runtime = Assert-TestRuntime $RuntimeDirectory $Instance
}
Assert-TestGameReady $gameDirectory
if (!(Test-Path -LiteralPath (Join-Path $runtime 'asmp-diag.dll'))) { throw 'Run build.ps1 first.' }
$launcher = Join-Path $build 'asmp-diag-launch.exe'
if (!(Test-Path -LiteralPath $launcher -PathType Leaf)) { throw 'Run build.ps1 first: launcher missing.' }
$otherGame = ''
if ($Instance -ne 'default') {
    $otherInstance = if ($Instance -eq 'client-a') { 'client-b' } else { 'client-a' }
    $otherGame = Join-Path (Get-TestGameDirectory $otherInstance) 'AlienShooter.exe'
}
Assert-TestGameIdle $gameDirectory -AllGames:($Instance -eq 'default') -AllowedOtherGame $otherGame
if ($Instance -ne 'default' -and !$RuntimeDirectory) { throw 'Pair instances require an isolated RuntimeDirectory.' }
if (!(Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Open Steam and sign in before starting the test copy.' }
$multiplayerMode = $Multiplayer -or !!$ServerAddress
if ($DummyActor -and $multiplayerMode) { throw 'DummyActor and multiplayer modes are mutually exclusive.' }
$monsterMode = $MonsterPuppet -or $MonsterFlying -or $MonsterCombat -or $MonsterDeath
if (@(@($MonsterPuppet, $MonsterFlying, $MonsterCombat, $MonsterDeath) | Where-Object { $_ }).Count -gt 1) {
    throw 'Choose MonsterPuppet, MonsterFlying, MonsterCombat or MonsterDeath.'
}
if ($MonsterReload -and !$MonsterFlying) { throw 'MonsterReload requires MonsterFlying.' }
if ($MonsterNetworkRole) {
    if ($monsterMode -or $MonsterReload -or $DummyActor -or !$ServerAddress -or $Instance -eq 'default') {
        throw 'MonsterNetworkRole requires an isolated pair instance and ServerAddress, without offline monster or dummy modes.'
    }
    if (($MonsterNetworkRole -eq 'host' -and $Instance -ne 'client-a') -or
        ($MonsterNetworkRole -eq 'client' -and $Instance -ne 'client-b')) {
        throw 'Monster network host is client-a; the replica client is client-b.'
    }
    if (Test-Path -LiteralPath (Join-Path $build 'network-session.json')) {
        throw 'Stop the older managed network session before running the monster pair.'
    }
}
if ($monsterMode -and ($DummyActor -or $multiplayerMode -or $Instance -ne 'default')) {
    throw 'Monster experiments require an offline default instance; omit DummyActor and multiplayer options.'
}
if ($monsterMode -and (Test-Path -LiteralPath (Join-Path $build 'network-session.json'))) {
    throw 'Stop the managed network session before running a monster experiment.'
}
$maps = Join-Path $gameDirectory 'Maps'
$backup = Join-Path $gameDirectory 'asmp-menu-backup'
$survivalScript = Join-Path $maps 'survive_01.lgc'
$monsterBackup = Join-Path $gameDirectory 'asmp-monster-backup'
$savedSurvival = Join-Path $monsterBackup 'survive_01.lgc'
if ($MonsterFlying -or $MonsterCombat -or $MonsterDeath -or $MonsterNetworkRole) {
    $monsterScriptName = if ($MonsterNetworkRole -eq 'host' -and $env:ASMP_DIAG_MONSTER_GALLERY -ne '1') { 'monster-host.lgc' }
        elseif ($MonsterCombat -or $MonsterDeath -or $MonsterNetworkRole) { 'monster-combat.lgc' } else { 'monster-flying.lgc' }
    $monsterScriptSource = Join-Path $PSScriptRoot $monsterScriptName
    if (!(Test-Path -LiteralPath (Join-Path $maps 'survive_01.map') -PathType Leaf) -or
        !(Test-Path -LiteralPath $survivalScript -PathType Leaf) -or
        !(Test-Path -LiteralPath $monsterScriptSource -PathType Leaf)) {
        throw "Monster experiment needs the copied survival map/script and diagnostics/$monsterScriptName."
    }
    New-Item -ItemType Directory -Path $monsterBackup -Force | Out-Null
    if (!(Test-Path -LiteralPath $savedSurvival)) { Copy-Item -LiteralPath $survivalScript -Destination $savedSurvival }
    $restoreMonsterOnFailure = $true
    Copy-Item -LiteralPath $monsterScriptSource -Destination $survivalScript -Force
} elseif (Test-Path -LiteralPath $savedSurvival -PathType Leaf) {
    Copy-Item -LiteralPath $savedSurvival -Destination $survivalScript -Force
}
if ($multiplayerMode) {
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    foreach ($file in @('MAINMENU.LGC', 'mainmenu.men')) {
        if (!(Test-Path -LiteralPath (Join-Path $backup $file))) {
            Copy-Item -LiteralPath (Join-Path $maps $file) -Destination (Join-Path $backup $file)
        }
    }
    $assets = Join-Path (Split-Path $PSScriptRoot -Parent) 'game\AlienShooter\Maps'
    foreach ($file in @('asmp_play.lgc', 'asmp_play.men', 'asmp_lobby.men')) {
        Copy-Item -LiteralPath (Join-Path $assets $file) -Destination (Join-Path $maps $file) -Force
    }
    Copy-Item -LiteralPath (Join-Path $assets 'mainmenu.lgc') -Destination (Join-Path $maps 'MAINMENU.LGC') -Force
    # The original menu replacement hook is expressed through the test-copy asset.
    Copy-Item -LiteralPath (Join-Path $assets 'asmp_mainmenu.men') -Destination (Join-Path $maps 'mainmenu.men') -Force
} elseif (Test-Path -LiteralPath $backup) {
    foreach ($file in @('MAINMENU.LGC', 'mainmenu.men')) {
        Copy-Item -LiteralPath (Join-Path $backup $file) -Destination (Join-Path $maps $file) -Force
    }
}
# Use the engine's own display settings in the test copy.
$configuration = Join-Path $gameDirectory 'AlienShooter.cfg'
$text = [IO.File]::ReadAllText($configuration)
$mode = if ($Fullscreen) { 1 } else { 0 }
$text = [regex]::Replace($text, '(?m)^DefaultScreenX=.*$', "DefaultScreenX=$Width")
$text = [regex]::Replace($text, '(?m)^DefaultScreenY=.*$', "DefaultScreenY=$Height")
$text = [regex]::Replace($text, '(?m)^DefaultFullScreenMode=.*$', "DefaultFullScreenMode=$mode")
[IO.File]::WriteAllText($configuration, $text, [Text.Encoding]::ASCII)
# Steam loads these saved values before falling back to CFG defaults.
$options = Join-Path $gameDirectory 'saves\options.ini'
$optionsText = if (Test-Path -LiteralPath $options) { [IO.File]::ReadAllText($options) } else { "[graph]`r`n" }
if ($optionsText -notmatch '(?m)^\[graph\]\s*$') { $optionsText += "`r`n[graph]`r`n" }
foreach ($entry in @(@('ScreenX', $Width), @('ScreenY', $Height), @('FullScreen', $mode))) {
    $key = $entry[0]
    $value = $entry[1]
    if ($optionsText -match "(?m)^$key=") {
        $optionsText = [regex]::Replace($optionsText, "(?m)^$key=.*$", "$key=$value")
    } else {
        $optionsText = [regex]::Replace($optionsText, '(?m)^\[graph\][^\S\r\n]*\r?$', "[graph]`r`n$key=$value")
    }
}
[IO.File]::WriteAllText($options, $optionsText, [Text.Encoding]::ASCII)
$stop = Join-Path $runtime 'asmp-diag.stop'
if (Test-Path -LiteralPath $stop) { Remove-Item -LiteralPath $stop }
$previousServer = $env:ASMP_DIAG_SERVER
$previousPort = $env:ASMP_DIAG_PORT
$previousName = $env:ASMP_DIAG_NAME
$previousDummy = $env:ASMP_DIAG_DUMMY
$previousMonster = $env:ASMP_DIAG_MONSTER
$previousMonsterNetwork = $env:ASMP_DIAG_MONSTER_NETWORK
$previousMultiplayer = $env:ASMP_DIAG_MULTIPLAYER
$previousLaunchResult = $env:ASMP_LAUNCH_RESULT
try {
    $env:ASMP_DIAG_SERVER = $ServerAddress
    $env:ASMP_DIAG_PORT = "$Port"
    $env:ASMP_DIAG_NAME = $Name
    $env:ASMP_LAUNCH_RESULT = $LaunchResultPath
    $env:ASMP_DIAG_MULTIPLAYER = if ($multiplayerMode) { '1' } else { '' }
    $env:ASMP_DIAG_DUMMY = if ($DummyActor) { '1' } else { '' }
    $env:ASMP_DIAG_MONSTER = if ($MonsterDeath) { '5' } elseif ($MonsterCombat) { '4' } elseif ($MonsterReload) { '3' } elseif ($MonsterFlying) { '2' } elseif ($MonsterPuppet) { '1' } else { '' }
    $env:ASMP_DIAG_MONSTER_NETWORK = $MonsterNetworkRole
    $launchArguments = @($game, (Join-Path $runtime 'asmp-diag.dll'))
    if (!$Fullscreen) { $launchArguments += @("$Width", "$Height") }
    Invoke-TestLauncher $launcher $launchArguments
} finally {
    $env:ASMP_DIAG_SERVER = $previousServer
    $env:ASMP_DIAG_PORT = $previousPort
    $env:ASMP_DIAG_NAME = $previousName
    $env:ASMP_DIAG_DUMMY = $previousDummy
    $env:ASMP_DIAG_MONSTER = $previousMonster
    $env:ASMP_DIAG_MONSTER_NETWORK = $previousMonsterNetwork
    $env:ASMP_DIAG_MULTIPLAYER = $previousMultiplayer
    $env:ASMP_LAUNCH_RESULT = $previousLaunchResult
}
if ($ServerAddress) { Write-Output 'The client connects and loads the server map automatically.' }
elseif ($Multiplayer) { Write-Output 'Open Multiplayer in the main menu; enter a nickname and IPv4:port, then connect.' }
elseif ($MonsterPuppet) { Write-Output 'Monster stage 1: survival loads automatically. One ground UNIT (optionally with one class-9 weapon part) follows a 30-second idle/run/stop/turn sequence near the player. Keep the player alive; inspect MONSTER records in the log. This mode changes that one monster, then restores native AI.' }
elseif ($MonsterFlying) { Write-Output 'Monster stage 2: survival loads with one flying UNIT drone (VID290) and no waves. Observe its 30-second idle/run/stop/turn sequence at native altitude. The stock survival script is backed up and restored on the next other-mode launch.' }
elseif ($MonsterCombat) { Write-Output 'Monster combat test: survival loads without waves. The diagnostic module creates one private ground UNIT (VID50 plus VID108), replays attack and death with AI and gameplay side effects suppressed, then checks native removal. The stock survival script is backed up and restored on the next other-mode launch.' }
elseif ($MonsterDeath) { Write-Output 'Monster death test: isolated combat sequence, then normal corpse and eight native gibs. Leave the player still for 35 seconds; inspect MONSTER_VISUAL records.' }
else { Write-Output 'Start a campaign or survival level for the read-only diagnostics.' }
if ($MonsterCombat -or $MonsterDeath) { Write-Output "Observe without firing or moving: idle, blocked damage, attack and death. Logs: $runtime\logs\asmp-diag-<PID>.log" }
else { Write-Output "Combat check: fire, switch weapons, collect ammo, take damage and heal. Logs: $runtime\logs\asmp-diag-<PID>.log" }
if ($DummyActor) { Write-Output 'Dummy test: enter a level, walk and turn. A second actor appears after 2 seconds, follows with an X offset of 80 and is removed after 60 seconds.' }
if ($MonsterReload) { Write-Output 'Separate cleanup case: reloads the same map once after 10 seconds, then releases control. Expect MONSTER_RELOAD completed cleanup=1; this is not a full phase-6 pass.' }
} catch {
    if ($restoreMonsterOnFailure) { Copy-Item -LiteralPath $savedSurvival -Destination $survivalScript -Force }
    throw
} finally { if ($sessionLock) { $sessionLock.Dispose() } }
