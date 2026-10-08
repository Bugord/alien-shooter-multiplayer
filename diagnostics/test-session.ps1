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
function Get-TestExeHash {
    '4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142'
}
function Assert-TestTree([string]$Directory) {
    foreach ($item in @(Get-ChildItem -LiteralPath $Directory -Recurse -Force)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in diagnostic copy: $($item.FullName)" }
    }
}
function Assert-TestGameReady([string]$Directory) {
    $null = Assert-TestDirectory $Directory $PSScriptRoot
    foreach ($file in @('asmp-diag-test.marker', 'AlienShooter.exe')) {
        $path = Assert-TestDirectory (Join-Path $Directory $file) $Directory
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Run prepare-test-game.ps1 first: test copy incomplete.' }
    }
    if ((Get-Content -LiteralPath (Join-Path $Directory 'asmp-diag-test.marker') -Raw).Trim() -ne 'ASMP diagnostic copy') { throw 'Invalid test-copy marker.' }
    if ((Get-FileHash -LiteralPath (Join-Path $Directory 'AlienShooter.exe') -Algorithm SHA256).Hash -ne (Get-TestExeHash)) { throw 'Unsupported test-copy EXE.' }
    # Menu/config/save writes must stay in the physical copy, including subdirectories.
    Assert-TestTree $Directory
}
function Assert-TestRuntime([string]$Directory, [string]$Instance) {
    $build = Join-Path $PSScriptRoot 'build'
    $runtime = Assert-TestDirectory $Directory (Join-Path $build 'two-client')
    if ([IO.Path]::GetFileName($runtime) -ine $Instance) { throw 'Runtime directory must end with the selected instance name.' }
    foreach ($file in @('asmp-runtime.marker', 'asmp-diag.dll', 'asmp-diag.stop', 'logs')) {
        $null = Assert-TestDirectory (Join-Path $runtime $file) $runtime
    }
    $marker = Join-Path $runtime 'asmp-runtime.marker'
    if (!(Test-Path -LiteralPath $marker -PathType Leaf)) { throw 'Runtime marker missing.' }
    if ((Get-Content -LiteralPath $marker -Raw).Trim() -ne 'ASMP isolated runtime') { throw 'Invalid runtime marker.' }
    if ((Get-FileHash -LiteralPath (Join-Path $runtime 'asmp-diag.dll') -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath (Join-Path $build 'asmp-diag.dll') -Algorithm SHA256).Hash) { throw 'Runtime DLL differs from current build.' }
    Assert-TestTree $runtime
    $runtime
}
function Assert-TestFreeSpace([string]$Directory, [long]$Bytes) {
    $drive = Get-PSDrive -Name ([IO.Path]::GetPathRoot($Directory).Substring(0, 1))
    if ($null -ne $drive.Free -and $drive.Free -lt $Bytes + 64MB) { throw 'Not enough free space for separate test resources.' }
}
function Invoke-TestLauncher([string]$Launcher, [string[]]$Arguments) {
    & $Launcher @Arguments
    if ($LASTEXITCODE) { throw "Diagnostic launcher failed ($LASTEXITCODE)." }
}
