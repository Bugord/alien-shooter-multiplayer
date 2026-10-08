$ErrorActionPreference = 'Stop'
$diagnosticsRoot = Split-Path $PSScriptRoot -Parent
function Check($Condition, [string]$Message) { if (!$Condition) { throw "CHECK failed: $Message" } }
function Expect-Throw([scriptblock]$Action, [string]$Message) {
    $thrown = $false
    try { & $Action | Out-Null } catch { $thrown = $true }
    Check $thrown $Message
}

# Run the actual preparation/start scripts in a disposable repository fixture.
# Only process enumeration, disk capacity, the supported EXE digest and native
# launcher are substituted. File copying, DLL hashing and settings writes are real.
$fixtureParent = Join-Path $diagnosticsRoot 'build\script-isolation-tests'
$fixture = Join-Path $fixtureParent ([guid]::NewGuid().ToString('N'))
$scripts = Join-Path $fixture 'diagnostics'
$source = Join-Path $fixture 'source'
$testState = [pscustomobject]@{ Processes = @(); Free = 10GB; FailLaunch = $false; FailInstance = ''; Peer = @(); Relay = $true; ValidRelay = $true; Launches = (New-Object 'Collections.Generic.List[object]') }
function Get-Process {
    param($Name, $Id, $ErrorAction)
    if ($Name -eq 'steam') { [pscustomobject]@{ Id = 10 } }
    elseif ($Name -eq 'state-peer') { $testState.Peer }
    elseif ($PSBoundParameters.ContainsKey('Id')) {
        $path = if ($testState.ValidRelay) { Join-Path $scripts 'build\asmp-server.exe' } else { 'C:\foreign-server.exe' }
        [pscustomobject]@{ Id = $Id; Path = $path }
    } else { $testState.Processes }
}
function Get-NetUDPEndpoint {
    param($LocalPort, $ErrorAction)
    if ($testState.Relay) { [pscustomobject]@{ OwningProcess = 99; LocalAddress = '0.0.0.0'; LocalPort = $LocalPort } }
}
function Get-PSDrive { param($Name) [pscustomobject]@{ Free = $testState.Free } }
$fixtureExeText = 'supported fixture executable'
$fixtureExeHash = $null
function Get-FileHash {
    param($LiteralPath, $Algorithm = 'SHA256')
    $result = Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath $LiteralPath -Algorithm $Algorithm
    if ($result.Hash -eq $fixtureExeHash) { $result.Hash = '4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142' }
    $result
}
$environmentNames = @('ASMP_DIAG_SERVER', 'ASMP_DIAG_PORT', 'ASMP_DIAG_NAME', 'ASMP_DIAG_DUMMY', 'ASMP_DIAG_MULTIPLAYER')
$previousEnvironment = @{}
foreach ($name in $environmentNames) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, "before-$name", 'Process')
}
function Check-Environment {
    foreach ($name in $environmentNames) { Check ([Environment]::GetEnvironmentVariable($name, 'Process') -eq "before-$name") "environment restored: $name" }
}
function Get-Snapshot([string]$Directory) {
    # Include all files so rejected preflights cannot quietly change menu/saves/stop.
    (@(Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName | ForEach-Object {
        $_.FullName + ':' + (Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath $_.FullName).Hash
    }) -join "`n")
}
try {
    New-Item -ItemType Directory -Path $scripts, (Join-Path $scripts 'build'), (Join-Path $source 'Maps'), (Join-Path $source 'saves'), (Join-Path $fixture 'game\AlienShooter\Maps') | Out-Null
    foreach ($file in @('test-session.ps1', 'prepare-test-game.ps1', 'prepare-test-runtimes.ps1', 'start-test.ps1', 'start-clients.ps1')) {
        Copy-Item -LiteralPath (Join-Path $diagnosticsRoot $file) -Destination (Join-Path $scripts $file)
    }
    Add-Content -LiteralPath (Join-Path $scripts 'test-session.ps1') -Value @'
function Invoke-TestLauncher([string]$Launcher, [string[]]$Arguments) {
    $testState.Launches.Add([pscustomobject]@{
        Launcher = $Launcher; Arguments = $Arguments
        Server = $env:ASMP_DIAG_SERVER; Port = $env:ASMP_DIAG_PORT
        Name = $env:ASMP_DIAG_NAME; Dummy = $env:ASMP_DIAG_DUMMY
        Multiplayer = $env:ASMP_DIAG_MULTIPLAYER
    })
    $instance = [IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Arguments[0]))
    if ($testState.FailLaunch -or ($testState.FailInstance -and $instance -eq $testState.FailInstance)) { throw 'Simulated launcher failure.' }
}
'@
    Set-Content -LiteralPath (Join-Path $source 'AlienShooter.exe') -Value $fixtureExeText -Encoding ascii
    $fixtureExeHash = (Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath (Join-Path $source 'AlienShooter.exe')).Hash
    Set-Content -LiteralPath (Join-Path $source 'AlienShooter.cfg') -Value "DefaultScreenX=800`r`nDefaultScreenY=600`r`nDefaultFullScreenMode=1" -Encoding ascii
    Set-Content -LiteralPath (Join-Path $source 'saves\options.ini') -Value "[graph]`r`nScreenX=800`r`nScreenY=600`r`nFullScreen=1" -Encoding ascii
    Set-Content -LiteralPath (Join-Path $source 'saves\save.ini') -Value 'owner=original' -Encoding ascii
    foreach ($file in @('MAINMENU.LGC', 'mainmenu.men')) { Set-Content -LiteralPath (Join-Path $source "Maps\$file") -Value 'native menu' -Encoding ascii }
    foreach ($file in @('asmp_play.lgc', 'asmp_play.men', 'mainmenu.lgc', 'asmp_mainmenu.men')) {
        Set-Content -LiteralPath (Join-Path $fixture "game\AlienShooter\Maps\$file") -Value "multiplayer $file" -Encoding ascii
    }
    Set-Content -LiteralPath (Join-Path $scripts 'build\asmp-diag.dll') -Value 'fixture DLL' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $scripts 'build\asmp-diag-launch.exe') -Value 'never executed' -Encoding ascii
    $sourceBefore = Get-Snapshot $source
    foreach ($instance in @('default', 'client-a', 'client-b')) { & (Join-Path $scripts 'prepare-test-game.ps1') -Source $source -Instance $instance | Out-Null }
    $a = Join-Path $scripts 'test-games\client-a'
    $b = Join-Path $scripts 'test-games\client-b'
    $defaultGame = Join-Path $scripts 'test-game'
    $pair = & (Join-Path $scripts 'prepare-test-runtimes.ps1')
    Check ($pair.ClientA -ne $pair.ClientB) 'separate runtimes returned'
    foreach ($runtime in @($pair.ClientA, $pair.ClientB)) {
        Check ((Get-FileHash -LiteralPath (Join-Path $runtime 'asmp-diag.dll')).Hash -eq $pair.DllSha256) 'both DLL copies verified before launch'
        Check (Test-Path -LiteralPath (Join-Path $runtime 'logs') -PathType Container) 'separate log directories prepared'
    }
    $secondPair = & (Join-Path $scripts 'prepare-test-runtimes.ps1')
    Check ($secondPair.Directory -ne $pair.Directory) 'fresh runtime directory on each preparation'
    $pairBefore = Get-Snapshot $pair.Directory
    $testState.Free = 0
    $pairCount = @(Get-ChildItem -LiteralPath (Split-Path $pair.Directory -Parent) -Directory).Count
    Expect-Throw { & (Join-Path $scripts 'prepare-test-runtimes.ps1') } 'insufficient disk space refused'
    Check (@(Get-ChildItem -LiteralPath (Split-Path $pair.Directory -Parent) -Directory).Count -eq $pairCount) 'space failure creates no runtime'
    $testState.Free = 10GB
    $testState.Processes = @([pscustomobject]@{ Id = 11; Path = (Join-Path $a 'AlienShooter.exe') })
    $aBefore = Get-Snapshot $a
    Expect-Throw { & (Join-Path $scripts 'prepare-test-game.ps1') -Source $source -Instance client-a } 'active game cannot be overwritten'
    Expect-Throw { & (Join-Path $scripts 'prepare-test-runtimes.ps1') } 'runtime preparation refuses active games'
    Check ((Get-Snapshot $a) -eq $aBefore) 'active preparation made no writes'
    Check ((Get-Snapshot $pair.Directory) -eq $pairBefore) 'old runtime untouched by preparation'
    $testState.Processes = @()

    $commonStop = Join-Path $scripts 'build\asmp-diag.stop'
    $aStop = Join-Path $pair.ClientA 'asmp-diag.stop'
    $bStop = Join-Path $pair.ClientB 'asmp-diag.stop'
    foreach ($path in @($commonStop, $aStop, $bStop)) { Set-Content -LiteralPath $path -Value 'stop' -Encoding ascii }
    $bBefore = Get-Snapshot $b
    & (Join-Path $scripts 'start-test.ps1') -Instance client-a -RuntimeDirectory $pair.ClientA -ServerAddress '127.0.0.1' -Port 27021 -Name 'LocalA' -Width 1024 -Height 768 | Out-Null
    Check ((Get-Snapshot $b) -eq $bBefore) 'A settings/menu writes leave B intact'
    Check (!(Test-Path -LiteralPath $aStop) -and (Test-Path -LiteralPath $bStop) -and (Test-Path -LiteralPath $commonStop)) 'A clears only its own stop file'
    Check ((Get-Content -LiteralPath (Join-Path $a 'asmp-menu-backup\MAINMENU.LGC') -Raw).Trim() -eq 'native menu') 'A owns native menu backup'
    Check-Environment
    $aBefore = Get-Snapshot $a
    & (Join-Path $scripts 'start-test.ps1') -Instance client-b -RuntimeDirectory $pair.ClientB -DummyActor -Port 27022 -Name 'LocalB' | Out-Null
    Check ((Get-Snapshot $a) -eq $aBefore) 'B settings writes leave A intact'
    Check (!(Test-Path -LiteralPath $bStop) -and (Test-Path -LiteralPath $commonStop)) 'B clears only its own stop file'
    Check ((Get-Content -LiteralPath (Join-Path $a 'AlienShooter.cfg') -Raw) -match 'DefaultScreenX=1024') 'A independent CFG'
    Check ((Get-Content -LiteralPath (Join-Path $b 'saves\options.ini') -Raw) -match 'ScreenX=800') 'B independent saved options'
    Check ((Get-Content -LiteralPath (Join-Path $b 'Maps\MAINMENU.LGC') -Raw).Trim() -eq 'native menu') 'B keeps native menu'
    Check-Environment
    $launchA = $testState.Launches[0]; $launchB = $testState.Launches[1]
    Check ($launchA.Name -eq 'LocalA' -and $launchA.Server -eq '127.0.0.1' -and $launchA.Port -eq '27021' -and $launchA.Multiplayer -eq '1' -and !$launchA.Dummy) 'A launcher environment'
    Check ($launchB.Name -eq 'LocalB' -and !$launchB.Server -and $launchB.Port -eq '27022' -and !$launchB.Multiplayer -and $launchB.Dummy -eq '1') 'B does not inherit A environment'
    Check ($launchA.Arguments[0] -eq (Join-Path $a 'AlienShooter.exe') -and $launchB.Arguments[1] -eq (Join-Path $pair.ClientB 'asmp-diag.dll')) 'launcher uses selected game and DLL paths'
    Set-Content -LiteralPath (Join-Path $a 'saves\save.ini') -Value 'owner=A' -Encoding ascii
    Check ((Get-Content -LiteralPath (Join-Path $b 'saves\save.ini') -Raw).Trim() -eq 'owner=original') 'independent save files'

    $testState.FailLaunch = $true
    Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance client-b -RuntimeDirectory $pair.ClientB -ServerAddress '127.0.0.2' -Name 'FailedB' } 'launcher failure propagated'
    Check-Environment
    $testState.FailLaunch = $false
    # The lock was released on failure, so a subsequent launch succeeds.
    & (Join-Path $scripts 'start-test.ps1') -Instance client-b -RuntimeDirectory $pair.ClientB | Out-Null
    Check ((Get-Content -LiteralPath (Join-Path $b 'Maps\MAINMENU.LGC') -Raw).Trim() -eq 'native menu') 'selected native menu restored after multiplayer launch'
    $launchCount = $testState.Launches.Count
    Set-Content -LiteralPath (Join-Path $a 'AlienShooter.exe') -Value 'unsupported executable' -Encoding ascii
    Set-Content -LiteralPath $aStop -Value 'stop' -Encoding ascii
    $aBefore = Get-Snapshot $a; $pairBefore = Get-Snapshot $pair.Directory
    Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance client-a -RuntimeDirectory $pair.ClientA -Multiplayer } 'bad EXE rejected before settings writes'
    Check ((Get-Snapshot $a) -eq $aBefore -and (Get-Snapshot $pair.Directory) -eq $pairBefore) 'bad EXE preflight changes no resources'
    Copy-Item -LiteralPath (Join-Path $source 'AlienShooter.exe') -Destination (Join-Path $a 'AlienShooter.exe') -Force
    Set-Content -LiteralPath (Join-Path $pair.ClientA 'asmp-diag.dll') -Value 'wrong DLL' -Encoding ascii
    $aBefore = Get-Snapshot $a
    Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance client-a -RuntimeDirectory $pair.ClientA } 'DLL mismatch refused'
    Check ((Get-Snapshot $a) -eq $aBefore -and (Test-Path -LiteralPath $aStop)) 'bad DLL rejected before settings/stop writes'
    Copy-Item -LiteralPath (Join-Path $scripts 'build\asmp-diag.dll') -Destination (Join-Path $pair.ClientA 'asmp-diag.dll') -Force
    Set-Content -LiteralPath (Join-Path $pair.ClientA 'asmp-runtime.marker') -Value 'incorrect marker' -Encoding ascii
    Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance client-a -RuntimeDirectory $pair.ClientA } 'invalid runtime marker refused'
    Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance client-a } 'shared runtime refused for pair'
    Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance default -RuntimeDirectory $pair.ClientB } 'default cannot use pair runtime'
    Check ($testState.Launches.Count -eq $launchCount) 'invalid preflights never invoke launcher'
    Set-Content -LiteralPath (Join-Path $pair.ClientA 'asmp-runtime.marker') -Value 'ASMP isolated runtime' -Encoding ascii
    $gameMarker = Join-Path $a 'asmp-diag-test.marker'
    Set-Content -LiteralPath $gameMarker -Value 'invalid game marker' -Encoding ascii
    $aBefore = Get-Snapshot $a
    Expect-Throw { & (Join-Path $scripts 'prepare-test-game.ps1') -Source $source -Instance client-a } 'invalid game marker refused during preparation'
    Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance client-a -RuntimeDirectory $pair.ClientA } 'invalid game marker refused during launch'
    Check ((Get-Snapshot $a) -eq $aBefore) 'invalid marker preflight changes no game files'
    Set-Content -LiteralPath $gameMarker -Value 'ASMP diagnostic copy' -Encoding ascii

    # A nested junction must not redirect save writes to the source installation.
    $junction = Join-Path $a 'saves\redirect'
    New-Item -ItemType Junction -Path $junction -Target (Join-Path $source 'saves') | Out-Null
    try {
        Expect-Throw { & (Join-Path $scripts 'prepare-test-game.ps1') -Source $source -Instance client-a } 'nested junction refused before copying'
        Expect-Throw { & (Join-Path $scripts 'start-test.ps1') -Instance client-a -RuntimeDirectory $pair.ClientA } 'nested junction refused before settings writes'
        Check ($testState.Launches.Count -eq $launchCount) 'junction launch never reaches native launcher'
    }
    finally {
        # Windows PowerShell 5.1 Remove-Item cannot reliably unlink a junction.
        # Nonrecursive Directory.Delete removes the link, preserving its target.
        [IO.Directory]::Delete($junction)
    }
    & (Join-Path $scripts 'start-test.ps1') | Out-Null
    Check ($testState.Launches[$testState.Launches.Count - 1].Arguments[0] -eq (Join-Path $defaultGame 'AlienShooter.exe')) 'default game path preserved'
    Check (!(Test-Path -LiteralPath $commonStop)) 'default clears common stop file'
    Check ((Get-Snapshot $source) -eq $sourceBefore) 'source installation unchanged'
    Check-Environment
    $launchCount = $testState.Launches.Count
    & (Join-Path $scripts 'start-clients.ps1') -NameA TeamA -NameB TeamB | Out-Null
    Check ($testState.Launches.Count -eq $launchCount + 2) 'client convenience command launches A and B'
    $launchA = $testState.Launches[$launchCount]; $launchB = $testState.Launches[$launchCount + 1]
    Check ($launchA.Name -eq 'TeamA' -and $launchB.Name -eq 'TeamB' -and $launchA.Server -eq '127.0.0.1' -and $launchB.Server -eq '127.0.0.1') 'convenience command shares server with distinct names'
    Check-Environment
    $launchCount = $testState.Launches.Count
    $testState.Relay = $false
    Expect-Throw { & (Join-Path $scripts 'start-clients.ps1') } 'missing local relay refused'
    $testState.Relay = $true; $testState.ValidRelay = $false
    Expect-Throw { & (Join-Path $scripts 'start-clients.ps1') } 'foreign port owner refused'
    $testState.ValidRelay = $true; $testState.Peer = @([pscustomobject]@{Id=12})
    Expect-Throw { & (Join-Path $scripts 'start-clients.ps1') } 'running diagnostic peer refused'
    Check ($testState.Launches.Count -eq $launchCount) 'server/peer conflicts do not launch clients'
    $testState.Peer = @(); $testState.FailInstance = 'client-b'
    Expect-Throw { & (Join-Path $scripts 'start-clients.ps1') } 'partial pair launch reports B failure'
    Check ($testState.Launches.Count -eq $launchCount + 2) 'partial pair failure preserves attempted A/B diagnostics'
    Check-Environment
    Write-Output 'Test resource isolation checks passed.'
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
    # Delete only this generated fixture, after checking its resolved boundary.
    if (Test-Path -LiteralPath $fixture) {
        $resolvedFixture = (Resolve-Path -LiteralPath $fixture).Path
        $resolvedParent = (Resolve-Path -LiteralPath $fixtureParent).Path
        if ([IO.Path]::GetDirectoryName($resolvedFixture) -ine $resolvedParent) { throw 'Unexpected fixture cleanup path.' }
        if (@(Get-ChildItem -LiteralPath $resolvedFixture -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Fixture still has a junction; preserve it for inspection.' }
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
