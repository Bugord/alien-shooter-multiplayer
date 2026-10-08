param(
    [switch]$Fullscreen,
    [switch]$DummyActor,
    [switch]$Multiplayer,
    [ValidateRange(640, 1920)][int]$Width = 800,
    [ValidateRange(480, 1080)][int]$Height = 600,
    [string]$ServerAddress = '',
    [ValidateRange(1, 65535)][int]$Port = 27020,
    [ValidateLength(1, 15)][string]$Name = 'SteamTester'
)
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$game = Join-Path $PSScriptRoot 'test-game\AlienShooter.exe'
if (!(Test-Path -LiteralPath $game)) { throw 'Run prepare-test-game.ps1 first.' }
if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'test-game\asmp-diag-test.marker'))) { throw 'Test-copy marker missing.' }
if (!(Test-Path -LiteralPath (Join-Path $build 'asmp-steam-diag.dll'))) { throw 'Run build.ps1 first.' }
if (@(Get-Process AlienShooter -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $game }).Count) {
    throw 'Close the existing diagnostic game copy before starting the next session.'
}
if (!(Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Open Steam and sign in before starting the test copy.' }
$multiplayerMode = $Multiplayer -or !!$ServerAddress
if ($DummyActor -and $multiplayerMode) { throw 'DummyActor and multiplayer modes are mutually exclusive.' }
$maps = Join-Path $PSScriptRoot 'test-game\Maps'
$backup = Join-Path $PSScriptRoot 'test-game\asmp-menu-backup'
if ($multiplayerMode) {
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    foreach ($file in @('MAINMENU.LGC', 'mainmenu.men')) {
        if (!(Test-Path -LiteralPath (Join-Path $backup $file))) {
            Copy-Item -LiteralPath (Join-Path $maps $file) -Destination (Join-Path $backup $file)
        }
    }
    $assets = Join-Path (Split-Path $PSScriptRoot -Parent) 'game\AlienShooter\Maps'
    foreach ($file in @('steam_asmp_play.lgc', 'asmp_play.men')) {
        Copy-Item -LiteralPath (Join-Path $assets $file) -Destination (Join-Path $maps $file) -Force
    }
    Copy-Item -LiteralPath (Join-Path $assets 'steam_mainmenu.lgc') -Destination (Join-Path $maps 'MAINMENU.LGC') -Force
    # The original menu replacement hook is expressed through the test-copy asset.
    Copy-Item -LiteralPath (Join-Path $assets 'asmp_mainmenu.men') -Destination (Join-Path $maps 'mainmenu.men') -Force
} elseif (Test-Path -LiteralPath $backup) {
    foreach ($file in @('MAINMENU.LGC', 'mainmenu.men')) {
        Copy-Item -LiteralPath (Join-Path $backup $file) -Destination (Join-Path $maps $file) -Force
    }
}
# Use the engine's own display settings in the test copy.
$configuration = Join-Path $PSScriptRoot 'test-game\AlienShooter.cfg'
$text = [IO.File]::ReadAllText($configuration)
$mode = if ($Fullscreen) { 1 } else { 0 }
$text = [regex]::Replace($text, '(?m)^DefaultScreenX=.*$', "DefaultScreenX=$Width")
$text = [regex]::Replace($text, '(?m)^DefaultScreenY=.*$', "DefaultScreenY=$Height")
$text = [regex]::Replace($text, '(?m)^DefaultFullScreenMode=.*$', "DefaultFullScreenMode=$mode")
[IO.File]::WriteAllText($configuration, $text, [Text.Encoding]::ASCII)
# Steam loads these saved values before falling back to CFG defaults.
$options = Join-Path $PSScriptRoot 'test-game\saves\options.ini'
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
$stop = Join-Path $build 'asmp-diag.stop'
if (Test-Path -LiteralPath $stop) { Remove-Item -LiteralPath $stop }
$previousServer = $env:ASMP_DIAG_SERVER
$previousPort = $env:ASMP_DIAG_PORT
$previousName = $env:ASMP_DIAG_NAME
$previousDummy = $env:ASMP_DIAG_DUMMY
$previousMultiplayer = $env:ASMP_DIAG_MULTIPLAYER
try {
    $env:ASMP_DIAG_SERVER = $ServerAddress
    $env:ASMP_DIAG_PORT = "$Port"
    $env:ASMP_DIAG_NAME = $Name
    $env:ASMP_DIAG_MULTIPLAYER = if ($multiplayerMode) { '1' } else { '' }
    $env:ASMP_DIAG_DUMMY = if ($DummyActor) { '1' } else { '' }
    $launchArguments = @($game, (Join-Path $build 'asmp-steam-diag.dll'))
    if (!$Fullscreen) { $launchArguments += @("$Width", "$Height") }
    & (Join-Path $build 'asmp-diag-launch.exe') @launchArguments
    if ($LASTEXITCODE) { throw "Diagnostic launcher failed ($LASTEXITCODE)." }
} finally {
    $env:ASMP_DIAG_SERVER = $previousServer
    $env:ASMP_DIAG_PORT = $previousPort
    $env:ASMP_DIAG_NAME = $previousName
    $env:ASMP_DIAG_DUMMY = $previousDummy
    $env:ASMP_DIAG_MULTIPLAYER = $previousMultiplayer
}
if ($ServerAddress) { Write-Output 'The client connects and loads the server map automatically.' }
elseif ($Multiplayer) { Write-Output 'Open Multiplayer in the main menu; enter a nickname and IPv4:port, then connect.' }
else { Write-Output 'Start a campaign or survival level for the read-only diagnostics.' }
Write-Output "Combat check: fire, switch weapons, collect ammo, take damage and heal. Logs: $build\logs\asmp-diag-<PID>.log"
if ($DummyActor) { Write-Output 'Dummy test: enter a level, walk and turn. A second actor appears after 2 seconds, follows with an X offset of 80 and is removed after 60 seconds.' }
