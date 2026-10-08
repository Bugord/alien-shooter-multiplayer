param([ValidateRange(1, 65535)][int]$Port = 27020)
$ErrorActionPreference = 'Stop'
$server = Join-Path $PSScriptRoot 'build\asmp-server.exe'
if (!(Test-Path -LiteralPath $server -PathType Leaf)) { throw 'Run build.ps1 -Server first.' }
$endpoints = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
if ($endpoints.Count) { throw "UDP port $Port is already in use (PID $($endpoints[0].OwningProcess)). Keep the existing server or stop your previous test first." }
Write-Output "Starting relay on UDP $Port. Run start-clients.sh in another Git Bash terminal. Ctrl+C stops this server."
# Foreground launch is intentional: the user's server terminal owns its lifetime.
& $server "$Port"
if ($LASTEXITCODE) { throw "Relay server exited ($LASTEXITCODE)." }
