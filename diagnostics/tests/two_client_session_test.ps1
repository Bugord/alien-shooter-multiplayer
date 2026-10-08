# Orchestration checks with substituted launch operations and harmless fake processes.
$ErrorActionPreference = 'Stop'
$diagnosticsRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $diagnosticsRoot 'test-session.ps1')
. (Join-Path $diagnosticsRoot 'two-client.ps1')
function Check($Condition, [string]$Message) { if (!$Condition) { throw "CHECK failed: $Message" } }
function Expect-Throw([scriptblock]$Action, [string]$Message) {
    $thrown = $false
    try { & $Action | Out-Null } catch { $thrown = $true }
    Check $thrown $Message
}
$fixtureParent = Join-Path $diagnosticsRoot 'build\two-client-tests'
$fixture = Join-Path $fixtureParent ([guid]::NewGuid().ToString('N'))
$script:TwoClientRoot = $fixture
$script:started = New-Object System.Collections.Generic.List[object]
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
# A copy of ping.exe stands in for AlienShooter.exe: it sleeps, has a real path and creation time.
$fakeGameRoot = Join-Path $fixture 'games'
$ping = Join-Path $env:SystemRoot 'System32\PING.EXE'
foreach ($instance in @('client-a', 'client-b')) {
    New-Item -ItemType Directory -Path (Join-Path $fakeGameRoot $instance) -Force | Out-Null
    Copy-Item -LiteralPath $ping -Destination (Join-Path $fakeGameRoot "$instance\AlienShooter.exe")
}
function Get-TestGameDirectory([string]$Instance = 'default') { Join-Path $fakeGameRoot $Instance }
function Start-Fake([string]$Exe) {
    $process = Start-Process -FilePath $Exe -ArgumentList '-n', '120', '127.0.0.1' -PassThru -WindowStyle Hidden
    $script:started.Add($process)
    $process
}
function Entry-For($Process, [string]$Role) {
    New-ProcessEntry $Role $Process.Id $Process.Path $Process.StartTime.ToUniversalTime().Ticks.ToString()
}
function New-FakeRuntime([string]$Name) {
    $directory = Join-Path $fixture $Name
    New-Item -ItemType Directory -Path (Join-Path $directory 'logs') -Force | Out-Null
    $directory
}
function New-Manifest($Server) {
    [pscustomobject]@{
        schema = 1; id = 'test'; state = 'starting'; address = '127.0.0.1'; port = 27999; server = $Server
        clients = [pscustomobject]@{
            A = [pscustomobject]@{ role = 'A'; name = 'LocalA'; pid = 0; path = ''; started = ''; runtime = (New-FakeRuntime 'a'); state = 'never-started'; message = '' }
            B = [pscustomobject]@{ role = 'B'; name = 'LocalB'; pid = 0; path = ''; started = ''; runtime = (New-FakeRuntime 'b'); state = 'never-started'; message = '' }
        }
    }
}
function Set-Identity($Client, $Process) {
    $Client.pid = $Process.Id
    $Client.path = $Process.Path
    $Client.started = $Process.StartTime.ToUniversalTime().Ticks.ToString()
}
try {
    # Manifest: atomic write, round trip, schema guard.
    Check ($null -eq (Read-TwoClientManifest)) 'missing manifest is null'
    $manifest = New-Manifest $null
    Write-TwoClientManifest $manifest
    $manifest.state = 'running'
    Write-TwoClientManifest $manifest
    Check ((Read-TwoClientManifest).state -eq 'running') 'manifest round trip and replace'
    Check (!(Test-Path -LiteralPath "$(Get-TwoClientManifestPath).tmp")) 'no temporary manifest left'
    Set-Content -LiteralPath (Get-TwoClientManifestPath) -Value '{"schema":99}'
    Expect-Throw { Read-TwoClientManifest } 'unknown schema refused'
    Set-Content -LiteralPath (Get-TwoClientManifestPath) -Value '{broken'
    Expect-Throw { Read-TwoClientManifest } 'corrupt manifest refused'
    Remove-Item -LiteralPath (Get-TwoClientManifestPath)

    # Concurrent commands.
    $lock = Enter-TwoClientLock
    try { Expect-Throw { Enter-TwoClientLock } 'second orchestration command refused' } finally { $lock.Dispose() }

    # Process identity, including PID reuse.
    $exe = Join-Path $fakeGameRoot 'client-a\AlienShooter.exe'
    $process = Start-Fake $exe
    $entry = Entry-For $process 'A'
    Check ((Get-EntryState $entry) -eq 'match') 'identity matches the live process'
    Check ((Get-EntryState (New-ProcessEntry 'A' $process.Id $entry.path '1')) -eq 'mismatch') 'creation time mismatch = PID reuse'
    Check ((Get-EntryState (New-ProcessEntry 'A' $process.Id (Join-Path $fakeGameRoot 'client-b\AlienShooter.exe') $entry.started)) -eq 'mismatch') 'image path mismatch'
    Check ((Get-EntryState (New-ProcessEntry 'A' 0 '' '')) -eq 'absent') 'no PID is absent'
    Check ((Get-EntryState (New-ProcessEntry 'A' 2147483000 $entry.path $entry.started)) -eq 'absent') 'dead PID is absent'

    # Log parsing.
    $logs = Join-Path (New-FakeRuntime 'logs-only') 'logs'
    $log = Join-Path $logs 'asmp-diag-77.log'
    Check ((Get-ClientReadiness $logs 77) -eq 'pending') 'no log yet is pending'
    Set-Content -LiteralPath $log -Value '# NET_READY id=1'
    Check ((Get-ClientReadiness $logs 77) -eq 'pending') 'connected without a map load is not ready'
    Add-Content -LiteralPath $log -Value '# SESSION map_load_result=1'
    Check ((Get-ClientReadiness $logs 77) -eq 'ready') 'handshake plus map load is ready'
    Set-Content -LiteralPath $log -Value "# NET_READY id=1`r`n# SESSION map_load_result=-1"
    Check ((Get-ClientReadiness $logs 77) -eq 'failed') 'failed map load detected'
    Set-Content -LiteralPath $log -Value '# NET_DISCONNECTED reason=2'
    Check ((Get-ClientReadiness $logs 77) -eq 'failed') 'disconnect detected'
    Check ((Get-CleanupState $logs 77) -eq 'pending') 'no stop line is pending'
    Set-Content -LiteralPath $log -Value '# STOP requested by asmp-diag.stop; hook_restore_result=1 cleanup=1'
    Check ((Get-CleanupState $logs 77) -eq 'done') 'cleanup=1'
    Set-Content -LiteralPath $log -Value '# STOP requested by asmp-diag.stop; hook_restore_result=0 cleanup=0'
    Check ((Get-CleanupState $logs 77) -eq 'failed') 'cleanup=0'

    # Client start with substituted launcher.
    $script:mode = 'ok'
    $script:TwoClientOps.StartClient = {
        param($Instance, $Runtime, $Address, $Port, $Name, $ResultPath)
        if ($script:mode -eq 'throw') { throw 'launcher crashed' }
        if ($script:mode -eq 'silent') { return }
        $fake = Start-Fake (Join-Path (Get-TestGameDirectory $Instance) 'AlienShooter.exe')
        $late = $script:mode -eq 'late-failure'
        [pscustomobject]@{ schema = 1; stage = $(if ($late) { 'loading-dll' } else { 'dll-loaded' }); ok = !$late; pid = $fake.Id; started = $fake.StartTime.ToUniversalTime().Ticks.ToString() } |
            ConvertTo-Json | Set-Content -LiteralPath $ResultPath
        $text = switch ($script:mode) { 'ok' { "# NET_READY id=1`r`n# SESSION map_load_result=1" } 'dropped' { '# NET_DISCONNECTED reason=2' } default { '' } }
        if ($text) { Set-Content -LiteralPath (Join-Path $Runtime "logs\asmp-diag-$($fake.Id).log") -Value $text }
    }
    $manifest = New-Manifest $null
    Write-TwoClientManifest $manifest
    Start-ManagedClient $manifest 'A' 5
    Check ($manifest.clients.A.state -eq 'running' -and (Get-EntryState $manifest.clients.A) -eq 'match') 'ready client recorded as running'
    Check ((Read-TwoClientManifest).clients.A.pid -eq $manifest.clients.A.pid) 'identity persisted to the manifest'
    $script:mode = 'late-failure'
    Expect-Throw { Start-ManagedClient $manifest 'B' 5 } 'launcher failure after process creation is an error'
    Check ($manifest.clients.B.state -eq 'failed' -and (Get-EntryState $manifest.clients.B) -eq 'match') 'created process identity kept after launcher failure'
    $script:mode = 'throw'
    $manifest.clients.B.pid = 0
    Expect-Throw { Start-ManagedClient $manifest 'B' 5 } 'launcher exception is an error'
    $script:mode = 'silent'
    Expect-Throw { Start-ManagedClient $manifest 'B' 5 } 'missing launcher result is an error'
    $script:mode = 'dropped'
    Expect-Throw { Start-ManagedClient $manifest 'B' 5 } 'disconnected client is not successful'
    $script:mode = 'never-ready'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Expect-Throw { Start-ManagedClient $manifest 'B' 1 } 'readiness timeout is an error'
    Check ($watch.Elapsed.TotalSeconds -lt 10) 'readiness wait is bounded'

    # Stop: clients first, server last, repeatable, identity-checked.
    $server = Start-Fake $exe
    $a = Start-Fake (Join-Path $fakeGameRoot 'client-a\AlienShooter.exe')
    $b = Start-Fake (Join-Path $fakeGameRoot 'client-b\AlienShooter.exe')
    $manifest = New-Manifest (Entry-For $server 'server')
    Set-Identity $manifest.clients.A $a
    Set-Identity $manifest.clients.B $b
    Write-TwoClientManifest $manifest
    Set-Content -LiteralPath (Join-Path $manifest.clients.A.runtime "logs\asmp-diag-$($a.Id).log") -Value '# STOP requested by asmp-diag.stop; hook_restore_result=1 cleanup=1'
    $unclean = @(Stop-TwoClientSession -TimeoutSeconds 1)
    Check ($unclean.Count -ge 1 -and ($unclean -join ' ') -match 'client B') 'a background client without ticks is reported'
    Check (Test-Path -LiteralPath (Join-Path $manifest.clients.A.runtime 'asmp-diag.stop')) 'stop file written for A'
    Check (Test-Path -LiteralPath (Join-Path $manifest.clients.B.runtime 'asmp-diag.stop')) 'stop file written for B'
    Check ((Get-EntryState (Entry-For $server 'server')) -eq 'match') 'server kept while a client is unclean'
    Check ((Read-TwoClientManifest).state -eq 'partial') 'manifest records the partial stop'
    Check ((Get-Process -Id $a.Id -ErrorAction SilentlyContinue) -and (Get-Process -Id $b.Id -ErrorAction SilentlyContinue)) 'stop never closes the games'
    Set-Content -LiteralPath (Join-Path $manifest.clients.B.runtime "logs\asmp-diag-$($b.Id).log") -Value '# STOP requested by asmp-diag.stop; hook_restore_result=1 cleanup=1'
    $unclean = @(Stop-TwoClientSession -TimeoutSeconds 1)
    Check ($unclean.Count -eq 0) 'second stop completes after B cleaned up'
    Start-Sleep -Milliseconds 300
    Check (!(Get-Process -Id $server.Id -ErrorAction SilentlyContinue)) 'server stopped last'
    Check ($null -ne (Get-Process -Id $a.Id -ErrorAction SilentlyContinue)) 'games remain open without -CloseGames'
    Check (@(Stop-TwoClientSession -TimeoutSeconds 1).Count -eq 0) 'stopping again is harmless'
    Check ((Read-TwoClientManifest).state -eq 'stopped') 'manifest reports stopped'
    $unclean = @(Stop-TwoClientSession -TimeoutSeconds 1 -CloseGames)
    Check ($unclean.Count -ge 1 -and ($unclean -join ' ') -match 'still open') 'console processes need -Force'
    $unclean = @(Stop-TwoClientSession -TimeoutSeconds 1 -CloseGames -Force)
    Check ($unclean.Count -eq 0) '-Force closes the games after a clean stop'
    Start-Sleep -Milliseconds 300
    Check (!(Get-Process -Id $a.Id -ErrorAction SilentlyContinue) -and !(Get-Process -Id $b.Id -ErrorAction SilentlyContinue)) 'games closed with -CloseGames -Force'

    # PID reuse: a different process under a recorded PID is never touched.
    $stranger = Start-Fake $exe
    $manifest = New-Manifest (New-ProcessEntry 'server' $stranger.Id $stranger.Path '1')
    Write-TwoClientManifest $manifest
    $unclean = @(Stop-TwoClientSession -TimeoutSeconds 1)
    Check (($unclean -join ' ') -match 'not verified') 'reused PID reported'
    Check ($null -ne (Get-Process -Id $stranger.Id -ErrorAction SilentlyContinue)) 'reused PID not terminated'
    Check ((Read-TwoClientManifest).state -eq 'partial') 'unverified server leaves a partial manifest'

    # Launcher machine-readable result on a pre-start failure (no game is created).
    $launcher = Join-Path $diagnosticsRoot 'build\asmp-diag-launch.exe'
    if (Test-Path -LiteralPath $launcher) {
        $resultPath = Join-Path $fixture 'launch-result.json'
        $previous = $env:ASMP_LAUNCH_RESULT
        try {
            $env:ASMP_LAUNCH_RESULT = $resultPath
            $arguments = @('"' + (Join-Path $fixture 'path with spaces\AlienShooter.exe') + '"', '"' + (Join-Path $fixture 'missing.dll') + '"')
            $run = Start-Process -FilePath $launcher -ArgumentList $arguments -Wait -PassThru -NoNewWindow -RedirectStandardError (Join-Path $fixture 'launcher.err')
            $launcherExit = $run.ExitCode
        } finally { $env:ASMP_LAUNCH_RESULT = $previous }
        Check ($launcherExit -ne 0) 'launcher rejects a missing DLL'
        $result = Read-LaunchResult $resultPath
        Check ($result -and $result.ok -eq $false -and $result.pid -eq 0 -and $result.stage -eq 'validate') 'launcher reports pre-start failure as JSON'
    }
    # Entry points refuse before changing anything (only when no real session is recorded).
    $realManifest = Join-Path $diagnosticsRoot 'build\two-client\session.json'
    if (!(Test-Path -LiteralPath $realManifest) -and (Test-Path -LiteralPath (Join-Path $diagnosticsRoot 'build\asmp-server.exe'))) {
        # Windows PowerShell 5.1 turns a child's stderr into a terminating error under Stop;
        # these calls are expected to fail and are judged by exit code and text.
        $ErrorActionPreference = 'Continue'
        $udp = New-Object Net.Sockets.UdpClient(0)
        try {
            $busyPort = ([Net.IPEndPoint]$udp.Client.LocalEndPoint).Port
            $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $diagnosticsRoot 'start-two-client-test.ps1') -Port $busyPort 2>&1 | Out-String
            Check ($LASTEXITCODE -ne 0 -and $output -match 'already in use') 'start refuses a busy UDP port'
            Check (!(Test-Path -LiteralPath $realManifest)) 'refused start leaves no manifest'
        } finally { $udp.Dispose() }
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $diagnosticsRoot 'stop-two-client-test.ps1') -Force 2>&1 | Out-String
        Check ($LASTEXITCODE -ne 0 -and $output -match 'CloseGames') '-Force alone is refused'
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $diagnosticsRoot 'stop-two-client-test.ps1') 2>&1 | Out-String
        Check ($LASTEXITCODE -eq 0 -and $output -match 'No two-client session') 'stop without a session is harmless'
        $ErrorActionPreference = 'Stop'
    }
    Write-Output 'Two-client session checks passed.'
} finally {
    foreach ($process in $script:started) { if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } }
    if (Test-Path -LiteralPath $fixture) {
        $resolved = (Resolve-Path -LiteralPath $fixture).Path
        if ([IO.Path]::GetDirectoryName($resolved) -ine (Resolve-Path -LiteralPath $fixtureParent).Path) { throw 'Unexpected fixture cleanup path.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
