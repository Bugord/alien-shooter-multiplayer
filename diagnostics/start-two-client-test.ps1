# Starts the local relay and two isolated real game clients, or restarts one exited client.
param(
    [ValidateRange(1, 65535)][int]$Port = 27020,
    [ValidateLength(1, 15)][string]$NameA = 'LocalA',
    [ValidateLength(1, 15)][string]$NameB = 'LocalB',
    [ValidateSet('', 'A', 'B')][string]$RestartClient = '',
    [ValidateRange(5, 600)][int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-session.ps1')
. (Join-Path $PSScriptRoot 'two-client.ps1')
$orchestrationLock = Enter-TwoClientLock
try {
    $build = Join-Path $PSScriptRoot 'build'
    foreach ($file in @('asmp-server.exe', 'asmp-diag.dll', 'asmp-diag-launch.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $build $file) -PathType Leaf)) { throw "Run build.ps1 -Server first: $file missing." }
    }
    $existing = Read-TwoClientManifest
    if ($RestartClient) {
        if (!$existing -or $existing.state -in @('stopped', 'failed')) { throw 'No running session to restart a client in. Start a new one first.' }
        if ((Get-EntryState $existing.server) -ne 'match') { throw 'The session server is not running; stop and start the pair again.' }
        $client = $existing.clients.$RestartClient
        if ((Get-EntryState $client) -ne 'absent') { throw "Client $RestartClient is still running or cannot be verified; close it first." }
        $name = if ($RestartClient -eq 'A') { $NameA } else { $NameB }
        $client.name = $name
        $client.state = 'starting'
        Start-ManagedClient $existing $RestartClient $TimeoutSeconds
        $existing.state = 'running'
        Write-TwoClientManifest $existing
        Write-Output "Client $RestartClient restarted (PID $($client.pid)); the server and the other client were left alone."
        return
    }

    # Refuse anything that could belong to another session before touching state.
    if (Test-Path -LiteralPath (Join-Path $build 'network-session.json')) { throw 'An older network test session is recorded; run stop-network-test.ps1 first.' }
    if (Get-Process state-peer -ErrorAction SilentlyContinue) { throw 'A diagnostic peer is running; finish that test first.' }
    if ($existing -and $existing.state -notin @('stopped')) {
        $live = @($existing.server) + @($existing.clients.A) + @($existing.clients.B) | Where-Object { (Get-EntryState $_) -ne 'absent' }
        if ($live) { throw 'The previous two-client session still has live or unverifiable processes; run stop-two-client-test.ps1.' }
    }
    if (@(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue).Count) { throw "UDP port $Port is already in use." }
    foreach ($instance in @('client-a', 'client-b')) {
        $directory = Get-TestGameDirectory $instance
        Assert-TestGameIdle $directory -AllGames
        Assert-TestGameReady $directory
    }
    if (!(Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Open Steam and sign in before starting the test copies.' }

    $pair = & (Join-Path $PSScriptRoot 'prepare-test-runtimes.ps1')
    $manifest = [pscustomobject]@{
        schema = $script:TwoClientSchema
        id = Split-Path $pair.Directory -Leaf
        state = 'starting'
        address = '127.0.0.1'
        port = $Port
        dllSha256 = $pair.DllSha256
        directory = $pair.Directory
        server = $null
        clients = [pscustomobject]@{
            A = [pscustomobject]@{ role = 'A'; name = $NameA; pid = 0; path = ''; started = ''; runtime = $pair.ClientA; state = 'never-started'; message = '' }
            B = [pscustomobject]@{ role = 'B'; name = $NameB; pid = 0; path = ''; started = ''; runtime = $pair.ClientB; state = 'never-started'; message = '' }
        }
    }
    Write-TwoClientManifest $manifest
    try {
        $process = & $script:TwoClientOps.StartServer $Port $pair.Directory
        $server = New-ProcessEntry 'server' $process.Id (Join-Path $build 'asmp-server.exe') $process.StartTime.ToUniversalTime().Ticks.ToString()
        $manifest.server = $server
        Write-TwoClientManifest $manifest
        if (!(& $script:TwoClientOps.WaitUdp $Port $process.Id 10)) { throw "The relay did not open UDP $Port; see $($pair.Directory)\server.err.log" }
        Start-ManagedClient $manifest 'A' $TimeoutSeconds
        Start-ManagedClient $manifest 'B' $TimeoutSeconds
        $manifest.state = 'running'
        Write-TwoClientManifest $manifest
    } catch {
        $failure = $_.Exception.Message
        $manifest.state = 'failed'
        Write-TwoClientManifest $manifest
        # Roll the mod back in anything already started; the games and the manifest stay for diagnosis.
        $left = @(Stop-TwoClientSession -TimeoutSeconds 5)
        $failedManifest = Read-TwoClientManifest
        $failedManifest.state = 'failed'
        Write-TwoClientManifest $failedManifest
        if ($left.Count) { $failure += " Cleanup incomplete: $($left -join '; ')" }
        throw $failure
    }
    Write-Output "Two-client session $($manifest.id) is running on 127.0.0.1:$Port."
    Write-Output "  Client A '$NameA' PID $($manifest.clients.A.pid)  logs: $($pair.ClientA)\logs"
    Write-Output "  Client B '$NameB' PID $($manifest.clients.B.pid)  logs: $($pair.ClientB)\logs"
    Write-Output "  Relay PID $($manifest.server.pid)  logs: $($pair.Directory)"
    Write-Output 'Stop: .\diagnostics\stop-two-client-test.ps1   (add -CloseGames to also close both windows)'
} finally { $orchestrationLock.Dispose() }
