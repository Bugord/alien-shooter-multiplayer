param(
    [string]$ServerAddress = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 27020,
    [ValidateLength(1, 15)][string]$NameA = 'LocalA',
    [ValidateLength(1, 15)][string]$NameB = 'LocalB'
)
$ErrorActionPreference = 'Stop'
if (Get-Process state-peer -ErrorAction SilentlyContinue) { throw 'A diagnostic peer is still running. Finish that test before launching two real clients.' }
if ($ServerAddress -eq '127.0.0.1') {
    $endpoints = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
    if (!$endpoints.Count) { throw "Start the local relay first: bash start-server.sh $Port" }
    $expectedServer = Join-Path $PSScriptRoot 'build\asmp-server.exe'
    foreach ($endpoint in $endpoints) {
        $process = Get-Process -Id $endpoint.OwningProcess -ErrorAction SilentlyContinue
        if (!$process -or $process.Path -ine $expectedServer) { throw "Cannot verify the local relay on UDP $Port; use matching process permissions and check the port owner." }
    }
}
$pair = & (Join-Path $PSScriptRoot 'prepare-test-runtimes.ps1')
Write-Output "Client A logs: $($pair.ClientA)\logs"
Write-Output "Client B logs: $($pair.ClientB)\logs"
try {
    & (Join-Path $PSScriptRoot 'start-test.ps1') -Instance client-a -RuntimeDirectory $pair.ClientA -ServerAddress $ServerAddress -Port $Port -Name $NameA
    & (Join-Path $PSScriptRoot 'start-test.ps1') -Instance client-b -RuntimeDirectory $pair.ClientB -ServerAddress $ServerAddress -Port $Port -Name $NameB
} catch {
    Write-Output 'Launch interrupted. Any game already started remains open; the log directories above are preserved.'
    throw
}
Write-Output 'Both launchers completed. Check both windows and logs for connection/map loading. Close the games normally after testing.'
