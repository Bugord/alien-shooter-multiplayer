# Shared path and lock checks for preparing/launching diagnostic copies.
function Get-TestGameDirectory([string]$Instance = 'default') {
    switch ($Instance) {
        'default' { Join-Path $PSScriptRoot 'test-game' }
        'client-a' { Join-Path $PSScriptRoot 'test-games\client-a' }
        'client-b' { Join-Path $PSScriptRoot 'test-games\client-b' }
        default { throw 'Unknown diagnostic instance.' }
    }
}
function Enter-TestSessionLock {
    $directory = Join-Path $PSScriptRoot 'build'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    try { [IO.File]::Open((Join-Path $directory 'test-session.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { throw 'Another diagnostic script is preparing or starting a session.' }
}
function Assert-TestGameIdle([string]$Directory, [switch]$AllGames, [string]$AllowedOtherGame = '') {
    $expected = Join-Path $Directory 'AlienShooter.exe'
    foreach ($process in @(Get-Process AlienShooter -ErrorAction SilentlyContinue)) {
        if (!$process.Path) { throw "Cannot identify AlienShooter PID $($process.Id); use a shell with matching permissions." }
        if ($AllGames -or $process.Path -ieq $expected -or ($AllowedOtherGame -and $process.Path -ine $AllowedOtherGame)) {
            throw "AlienShooter PID $($process.Id) is running. Leave other sessions open; exit the relevant game before continuing."
        }
    }
}
function Assert-TestDirectory([string]$Path, [string]$Root) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if (!$full.StartsWith($rootFull + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Path outside diagnostic root: $Path" }
    # Refuse junctions/symlinks, including ancestors through the repository.
    for ($current = $full; $current; $current = [IO.Path]::GetDirectoryName($current)) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in diagnostic path: $current" }
        }
    }
    $full
}
