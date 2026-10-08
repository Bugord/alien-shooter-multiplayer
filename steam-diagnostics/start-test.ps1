param(
    [switch]$Fullscreen,
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
try {
    $env:ASMP_DIAG_SERVER = $ServerAddress
    $env:ASMP_DIAG_PORT = "$Port"
    $env:ASMP_DIAG_NAME = $Name
    $launchArguments = @($game, (Join-Path $build 'asmp-steam-diag.dll'))
    if (!$Fullscreen) { $launchArguments += @("$Width", "$Height") }
    & (Join-Path $build 'asmp-diag-launch.exe') @launchArguments
    if ($LASTEXITCODE) { throw "Diagnostic launcher failed ($LASTEXITCODE)." }
} finally {
    $env:ASMP_DIAG_SERVER = $previousServer
    $env:ASMP_DIAG_PORT = $previousPort
    $env:ASMP_DIAG_NAME = $previousName
}
Write-Output "Start a level; fire, switch weapons, collect ammo, take damage and heal. Inspect $build\logs\asmp-diag-<PID>.log"
