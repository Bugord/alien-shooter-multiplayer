$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$sessionFile = Join-Path $build 'network-session.json'
if (!(Test-Path -LiteralPath $sessionFile)) { return }
# Windows PowerShell 5.1 emits a JSON array as one object; ForEach-Object unrolls it.
$processes = @(Get-Content -LiteralPath $sessionFile -Raw | ConvertFrom-Json | ForEach-Object { $_ })
foreach ($saved in $processes) {
    $process = Get-Process -Id $saved.Id -ErrorAction SilentlyContinue
    if (!$process) { continue }
    $allowed = @((Join-Path $build 'asmp-server.exe'), (Join-Path $build 'state-peer.exe'))
    if ($saved.Path -notin $allowed -or $process.Path -ine $saved.Path -or
        $process.StartTime.ToUniversalTime().Ticks.ToString() -ne $saved.Started) {
        throw "Process $($saved.Id) does not match this test session; left running."
    }
    Stop-Process -InputObject $process
}
Remove-Item -LiteralPath $sessionFile
Write-Output 'Local network test processes stopped.'
