param([switch]$Mirror, [switch]$FireOnce, [ValidateRange(1, 65535)][int]$Port = 27020)
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$sessionFile = Join-Path $build 'network-session.json'
if (Test-Path -LiteralPath $sessionFile) { throw 'Run stop-network-test.ps1 before starting another network session.' }
if ($FireOnce -and !$Mirror) { throw 'FireOnce requires Mirror.' }
foreach ($name in @('asmp-server.exe', 'state-peer.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $build $name))) { throw 'Run build.ps1 -Server first.' }
}
if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) { throw "UDP port $Port is already in use." }
$logs = Join-Path $build 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$processes = @()
try {
    $serverPath = Join-Path $build 'asmp-server.exe'
    $server = Start-Process -FilePath $serverPath -ArgumentList "$Port" -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $logs "server-$stamp.log") -RedirectStandardError (Join-Path $logs "server-$stamp.err.log")
    $processes += @{ Id = $server.Id; Path = $serverPath; Started = $server.StartTime.ToUniversalTime().Ticks.ToString() }
    Start-Sleep -Milliseconds 300
    if ($server.HasExited -or !(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue | Where-Object OwningProcess -eq $server.Id)) { throw 'Local UDP server did not start.' }
    $peerPath = Join-Path $build 'state-peer.exe'
    $peerArguments = @('127.0.0.1', "$Port", '600')
    if ($Mirror) { $peerArguments += '--mirror' }
    if ($FireOnce) { $peerArguments += '--fire-once' }
    $peer = Start-Process -FilePath $peerPath -ArgumentList $peerArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $logs "observer-$stamp.log") -RedirectStandardError (Join-Path $logs "observer-$stamp.err.log")
    $processes += @{ Id = $peer.Id; Path = $peerPath; Started = $peer.StartTime.ToUniversalTime().Ticks.ToString() }
    $processes | ConvertTo-Json | Set-Content -LiteralPath $sessionFile
    & (Join-Path $PSScriptRoot 'start-test.ps1') -ServerAddress '127.0.0.1' -Port $Port
    if ($Mirror) { Write-Output 'Mirror follows your player at X+80 and replays received shots for up to 10 minutes.' }
    else { Write-Output 'Observer receives state for up to 10 minutes; it does not spawn an actor.' }
    Write-Output "Peer log: $logs\observer-$stamp.log"
    Write-Output 'After the game test, run stop-network-test.ps1 to stop the local server and observer.'
} catch {
    if ($processes.Count) {
        if (!(Test-Path -LiteralPath $sessionFile)) { $processes | ConvertTo-Json | Set-Content -LiteralPath $sessionFile }
        & (Join-Path $PSScriptRoot 'stop-network-test.ps1')
    }
    throw
}
