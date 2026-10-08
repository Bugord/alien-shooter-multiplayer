# Prepare both DLL directories before either client starts; does not launch games.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-session.ps1')
$sessionLock = Enter-TestSessionLock
try {
    foreach ($instance in @('client-a', 'client-b')) {
        $gameDirectory = Get-TestGameDirectory $instance
        Assert-TestGameIdle $gameDirectory -AllGames
        Assert-TestGameReady $gameDirectory
    }
    $build = Join-Path $PSScriptRoot 'build'
    $dll = Assert-TestDirectory (Join-Path $build 'asmp-diag.dll') $build
    if (!(Test-Path -LiteralPath $dll -PathType Leaf)) { throw 'Run build.ps1 first.' }
    $hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    $pairRoot = Assert-TestDirectory (Join-Path $build 'two-client') $build
    Assert-TestFreeSpace $pairRoot (2 * (Get-Item -LiteralPath $dll).Length)
    $id = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N'))
    $directory = Assert-TestDirectory (Join-Path $pairRoot $id) $pairRoot
    # Never overwrite an earlier runtime: its pinned DLL/logs may still be in use.
    New-Item -ItemType Directory -Path $directory | Out-Null
    foreach ($instance in @('client-a', 'client-b')) {
        $runtime = Join-Path $directory $instance
        New-Item -ItemType Directory -Path (Join-Path $runtime 'logs') | Out-Null
        Copy-Item -LiteralPath $dll -Destination (Join-Path $runtime 'asmp-diag.dll')
        if ((Get-FileHash -LiteralPath (Join-Path $runtime 'asmp-diag.dll') -Algorithm SHA256).Hash -ne $hash) { throw "Runtime DLL verification failed: $instance" }
    }
    # Mark complete only after both copies have been verified.
    foreach ($instance in @('client-a', 'client-b')) {
        Set-Content -LiteralPath (Join-Path $directory "$instance\asmp-runtime.marker") -Value 'ASMP isolated runtime' -Encoding ascii
    }
    [pscustomobject]@{
        Directory = $directory
        ClientA = Join-Path $directory 'client-a'
        ClientB = Join-Path $directory 'client-b'
        DllSha256 = $hash
    }
} finally { $sessionLock.Dispose() }
