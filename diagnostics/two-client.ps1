# Shared helpers for the managed two-client session (server + client A + client B).
# Dot-source after test-session.ps1. Tests replace $script:TwoClientOps entries.
$script:TwoClientSchema = 1
$script:TwoClientRoot = Join-Path $PSScriptRoot 'build\two-client'

function Get-TwoClientManifestPath { Join-Path $script:TwoClientRoot 'session.json' }

function Enter-TwoClientLock {
    New-Item -ItemType Directory -Path $script:TwoClientRoot -Force | Out-Null
    try { [IO.File]::Open((Join-Path $script:TwoClientRoot 'orchestration.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { throw 'Another two-client start or stop command is running.' }
}

# The manifest is replaced atomically so a crash never leaves half a file.
function Write-TwoClientManifest($Manifest) {
    New-Item -ItemType Directory -Path $script:TwoClientRoot -Force | Out-Null
    $path = Get-TwoClientManifestPath
    $temporary = "$path.tmp"
    [IO.File]::WriteAllText($temporary, ($Manifest | ConvertTo-Json -Depth 8), [Text.Encoding]::ASCII)
    if (Test-Path -LiteralPath $path) { [IO.File]::Replace($temporary, $path, [NullString]::Value) }
    else { [IO.File]::Move($temporary, $path) }
}

function Read-TwoClientManifest {
    $path = Get-TwoClientManifestPath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { $manifest = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } catch { throw "Session manifest is unreadable: $path" }
    if ($manifest.schema -ne $script:TwoClientSchema) { throw 'Session manifest has an unknown schema; inspect it before continuing.' }
    $manifest
}

function New-ProcessEntry([string]$Role, [int]$ProcessId, [string]$Path, [string]$Started) {
    [pscustomobject]@{ role = $Role; pid = $ProcessId; path = $Path; started = $Started }
}

# absent: no such process; match: same PID, image and creation time;
# mismatch: PID reused by something else; unknown: identity unreadable (never treated as free).
function Get-EntryState($Entry) {
    if (!$Entry -or !$Entry.pid) { return 'absent' }
    $process = Get-Process -Id ([int]$Entry.pid) -ErrorAction SilentlyContinue
    if (!$process) { return 'absent' }
    try {
        $path = $process.Path
        $started = $process.StartTime.ToUniversalTime().Ticks.ToString()
    } catch { return 'unknown' }
    if (!$path) { return 'unknown' }
    if ($path -ine $Entry.path -or $started -ne $Entry.started) { return 'mismatch' }
    'match'
}

function Read-LaunchResult([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try { $result = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch { return $null }
    if ($result.schema -ne 1) { return $null }
    $result
}

function Read-SharedText([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite, Delete')
    try { (New-Object IO.StreamReader($stream)).ReadToEnd() } finally { $stream.Dispose() }
}

# ready needs both the transport handshake and an accepted map load; loader success alone is not enough.
function Get-ClientReadiness([string]$LogsDirectory, [int]$ProcessId) {
    $text = Read-SharedText (Join-Path $LogsDirectory "asmp-diag-$ProcessId.log")
    if ($text -match '# NET_DISCONNECTED' -or $text -match '# SESSION map_load_result=-') { return 'failed' }
    if ($text -match '# NET_READY' -and $text -match '# SESSION map_load_result=1') { return 'ready' }
    'pending'
}

# done: hooks restored (cleanup=1); failed: cleanup=0; pending: no stop line yet (a background game may not tick).
function Get-CleanupState([string]$LogsDirectory, [int]$ProcessId) {
    $text = Read-SharedText (Join-Path $LogsDirectory "asmp-diag-$ProcessId.log")
    $match = [regex]::Match($text, '# STOP requested[^\r\n]*cleanup=(\d)')
    if (!$match.Success) { return 'pending' }
    if ($match.Groups[1].Value -eq '1') { 'done' } else { 'failed' }
}

function Wait-Until([scriptblock]$Condition, [int]$TimeoutSeconds, [int]$PollMilliseconds = 250) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $value = & $Condition
        if ($value) { return $value }
        Start-Sleep -Milliseconds $PollMilliseconds
    } while ([DateTime]::UtcNow -lt $deadline)
    $null
}

$script:TwoClientOps = @{
    StartServer = {
        param([int]$Port, [string]$LogDirectory)
        $server = Join-Path $PSScriptRoot 'build\asmp-server.exe'
        Start-Process -FilePath $server -ArgumentList "$Port" -PassThru -WindowStyle Minimized `
            -RedirectStandardOutput (Join-Path $LogDirectory 'server.out.log') -RedirectStandardError (Join-Path $LogDirectory 'server.err.log')
    }
    WaitUdp = {
        param([int]$Port, [int]$ProcessId, [int]$TimeoutSeconds)
        Wait-Until { @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue | Where-Object OwningProcess -eq $ProcessId).Count -gt 0 } $TimeoutSeconds
    }
    StartClient = {
        param([string]$Instance, [string]$Runtime, [string]$Address, [int]$Port, [string]$Name, [string]$ResultPath)
        & (Join-Path $PSScriptRoot 'start-test.ps1') -Instance $Instance -RuntimeDirectory $Runtime -ServerAddress $Address `
            -Port $Port -Name $Name -LaunchResultPath $ResultPath | Out-Null
    }
}

function Set-ClientState($Manifest, [string]$Role, [string]$State, [string]$Message = '') {
    $client = $Manifest.clients.$Role
    $client | Add-Member -NotePropertyName state -NotePropertyValue $State -Force
    $client | Add-Member -NotePropertyName message -NotePropertyValue $Message -Force
    Write-TwoClientManifest $Manifest
}

# Launch one client, record its identity immediately, then wait for NET_READY + map load.
function Start-ManagedClient($Manifest, [string]$Role, [int]$TimeoutSeconds) {
    $client = $Manifest.clients.$Role
    $instance = if ($Role -eq 'A') { 'client-a' } else { 'client-b' }
    $resultPath = Join-Path $client.runtime 'launch-result.json'
    if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath }
    $game = Join-Path (Get-TestGameDirectory $instance) 'AlienShooter.exe'
    $failure = $null
    try { & $script:TwoClientOps.StartClient $instance $client.runtime $Manifest.address $Manifest.port $client.name $resultPath }
    catch { $failure = $_.Exception.Message }
    $result = Read-LaunchResult $resultPath
    # Keep the identity of a created process even when a later launcher stage failed.
    if ($result -and $result.pid) {
        $client.pid = [int]$result.pid
        $client.path = $game
        $client.started = [string]$result.started
    }
    if ($failure -or !$result -or !$result.ok) {
        $detail = if ($failure) { $failure } elseif ($result) { "launcher stopped at stage '$($result.stage)'" } else { 'launcher wrote no result' }
        Set-ClientState $Manifest $Role 'failed' $detail
        throw "Client $Role did not start: $detail"
    }
    if ((Get-EntryState $client) -ne 'match') {
        Set-ClientState $Manifest $Role 'failed' 'process identity does not match the launcher result'
        throw "Client $Role identity check failed (PID $($client.pid))."
    }
    Set-ClientState $Manifest $Role 'starting'
    $logs = Join-Path $client.runtime 'logs'
    $state = Wait-Until {
        if ((Get-EntryState $client) -ne 'match') { return 'exited' }
        $readiness = Get-ClientReadiness $logs ([int]$client.pid)
        if ($readiness -ne 'pending') { $readiness }
    } $TimeoutSeconds
    if ($state -ne 'ready') {
        $reason = if ($state) { "client reported '$state'" } else { "no NET_READY and map load within $TimeoutSeconds s" }
        Set-ClientState $Manifest $Role 'failed' $reason
        throw "Client $Role is not connected: $reason. Log: $logs\asmp-diag-$($client.pid).log"
    }
    Set-ClientState $Manifest $Role 'running'
}

# Repeatable, identity-checked stop. Returns a list of resources that could not be cleaned up.
function Stop-TwoClientSession([int]$TimeoutSeconds = 15, [switch]$CloseGames, [switch]$Force) {
    $manifest = Read-TwoClientManifest
    if (!$manifest) { return @() }
    $unclean = New-Object System.Collections.Generic.List[string]
    $waiting = @{}
    foreach ($role in @('A', 'B')) {
        $client = $manifest.clients.$role
        switch (Get-EntryState $client) {
            'absent' { if ($client.state -notin @('stopped', 'never-started')) { $client | Add-Member -NotePropertyName state -NotePropertyValue 'exited' -Force } }
            'match' {
                Set-Content -LiteralPath (Join-Path $client.runtime 'asmp-diag.stop') -Value 'stop' -Encoding ascii
                $waiting[$role] = $client
            }
            default { $unclean.Add("client $role PID $($client.pid): identity not verified, left untouched") }
        }
    }
    $pending = @($waiting.Keys)
    $null = Wait-Until {
        $pending = @($pending | Where-Object {
            (Get-EntryState $waiting[$_]) -eq 'match' -and (Get-CleanupState (Join-Path $waiting[$_].runtime 'logs') ([int]$waiting[$_].pid)) -eq 'pending'
        })
        $pending.Count -eq 0
    } $TimeoutSeconds
    foreach ($role in $waiting.Keys) {
        $client = $waiting[$role]
        if ((Get-EntryState $client) -ne 'match') { $client | Add-Member -NotePropertyName state -NotePropertyValue 'exited' -Force; continue }
        $cleanup = Get-CleanupState (Join-Path $client.runtime 'logs') ([int]$client.pid)
        if ($cleanup -eq 'done') { $client | Add-Member -NotePropertyName state -NotePropertyValue 'mod-stopped' -Force }
        elseif ($cleanup -eq 'failed') { $unclean.Add("client $role PID $($client.pid): hook cleanup failed") }
        else { $unclean.Add("client $role PID $($client.pid): no cleanup within $TimeoutSeconds s; bring that window to the front and run stop again") }
    }
    if ($CloseGames) {
        foreach ($role in @('A', 'B')) {
            $client = $manifest.clients.$role
            if ($client.state -ne 'mod-stopped' -or (Get-EntryState $client) -ne 'match') { continue }
            $process = Get-Process -Id ([int]$client.pid) -ErrorAction SilentlyContinue
            if ($process) { $null = $process.CloseMainWindow() }
            $closed = Wait-Until { (Get-EntryState $client) -ne 'match' } $TimeoutSeconds
            if ($closed) { $client | Add-Member -NotePropertyName state -NotePropertyValue 'stopped' -Force }
            elseif ($Force -and (Get-EntryState $client) -eq 'match') {
                Stop-Process -Id ([int]$client.pid) -Force
                $client | Add-Member -NotePropertyName state -NotePropertyValue 'stopped' -Force
            } else { $unclean.Add("client $role PID $($client.pid): still open (use -Force to terminate it)") }
        }
    }
    if ($unclean.Count -eq 0) {
        switch (Get-EntryState $manifest.server) {
            'match' { Stop-Process -Id ([int]$manifest.server.pid); $manifest.server | Add-Member -NotePropertyName state -NotePropertyValue 'stopped' -Force }
            'absent' { $manifest.server | Add-Member -NotePropertyName state -NotePropertyValue 'stopped' -Force }
            default { $unclean.Add("server PID $($manifest.server.pid): identity not verified, left untouched") }
        }
    } else {
        $unclean.Add('server left running until clients are clean')
    }
    $manifest.state = if ($unclean.Count) { 'partial' } else { 'stopped' }
    $manifest | Add-Member -NotePropertyName unclean -NotePropertyValue @($unclean) -Force
    Write-TwoClientManifest $manifest
    @($unclean)
}
