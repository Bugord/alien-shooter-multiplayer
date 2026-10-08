param([switch]$SkipTests, [switch]$Server)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ tools are required.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'No installed Visual Studio x86 C++ toolchain found.' }
$devCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
$command = 'call "{0}" -arch=x86 -host_arch=x64 >nul && set' -f $devCmd
$vsEnvironment = & $env:COMSPEC /d /s /c $command
if ($LASTEXITCODE) { throw 'Visual Studio environment setup failed.' }
foreach ($line in $vsEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
New-Item -ItemType Directory -Path $build -Force | Out-Null
function Compile([string[]]$Arguments) {
    & cl.exe @Arguments
    if ($LASTEXITCODE) { throw "Compilation failed ($LASTEXITCODE)." }
}
$flags = @('/nologo', '/std:c11', '/W4', '/WX', '/O2', '/MT', '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN')
$repo = Split-Path $root -Parent
$netIncludes = @("/I$repo\common\epnet\include", "/I$repo\common\epnet\src\common", "/I$repo\common\src")
$netCommon = @(Get-ChildItem "$repo\common\epnet\src\common\*.c" | ForEach-Object FullName)
$netClient = @("$repo\common\epnet\src\client\epnet_client.c")
$actorSources = @("$repo\asmp-dll\src\game\actor.c", "$repo\asmp-dll\src\game\actor_api.c")
$multiplayerSources = @("$repo\asmp-dll\src\multiplayer\multiplayer.c", "$repo\asmp-dll\src\game\action_hook.c", "$repo\asmp-dll\src\game\ui.c", "$repo\asmp-dll\src\game\display_hook.c", "$repo\asmp-dll\src\multiplayer\session.c")
$hookSources = @("$repo\asmp-dll\src\game\slot_hook.c", "$repo\asmp-dll\src\game\tick_hook.c", "$repo\asmp-dll\src\game\world_hook.c")
$runtimeSource = "$repo\asmp-dll\src\multiplayer\runtime.c"
Push-Location $build
try {
    # Compile the existing networking library separately with its warning level.
    Compile (@('/nologo', '/std:c11', '/W3', '/O2', '/MT', '/DWIN32_LEAN_AND_MEAN', '/c') + $netIncludes + $netCommon + $netClient)
    $netObjects = @($netCommon + $netClient | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) + '.obj' })
    Compile ($flags + $actorSources + $multiplayerSources + $hookSources + @($runtimeSource) + $netIncludes + @('/LD', "$root\src\diag.c", "$repo\asmp-dll\src\multiplayer\client\state_client.c", "$repo\asmp-dll\src\game\probe.c", "$root\src\diag_tick.c", "$root\src\dummy_actor.c", "$root\src\hash.c", '/Fe:asmp-diag.dll') + $netObjects + @('/link', 'ws2_32.lib', 'advapi32.lib', 'bcrypt.lib', '/MACHINE:X86', '/INCREMENTAL:NO'))
    Compile ($flags + $netIncludes + @("$root\src\state_peer.c", "$repo\asmp-dll\src\multiplayer\client\state_client.c", '/Fe:state-peer.exe') + $netObjects + @('/link', 'ws2_32.lib', 'advapi32.lib', '/MACHINE:X86'))
    Compile ($flags + @("$root\src\launcher.c", "$root\src\window_mode.c", "$root\src\hash.c", '/Fe:asmp-diag-launch.exe', '/link', 'bcrypt.lib', 'user32.lib', '/MACHINE:X86', '/INCREMENTAL:NO'))
    if (!$SkipTests) {
        Compile ($flags + @("$root\tests\window_mode_test.c", "$root\src\window_mode.c", '/Fe:window-mode-test.exe', '/link', '/MACHINE:X86'))
        & .\window-mode-test.exe
        if ($LASTEXITCODE) { throw 'Window mode checks failed.' }
        $serverSupport = @("$repo\asmp-server\src\server.c", "$repo\common\epnet\src\server\epnet_server.c", "$repo\common\src\utils\mem\mem.c", "$repo\common\src\utils\time\time.c")
        Compile (@('/nologo', '/std:c11', '/W3', '/O2', '/MT', '/DWIN32_LEAN_AND_MEAN', '/c') + $netIncludes + $serverSupport)
        $serverObjects = @($serverSupport | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) + '.obj' })
        Compile ($flags + $netIncludes + @("$root\tests\state_sync_test.c", "$repo\asmp-dll\src\multiplayer\client\state_client.c", '/Fe:state-sync-test.exe') + $netObjects + $serverObjects + @('/link', 'ws2_32.lib', 'advapi32.lib', '/MACHINE:X86'))
        & .\state-sync-test.exe
        if ($LASTEXITCODE) { throw 'State sync checks failed.' }
        Compile ($flags + @("$root\tests\probe_test.c", "$repo\asmp-dll\src\game\probe.c", '/Fe:probe-test.exe', '/link', '/MACHINE:X86'))
        Compile ($flags + @("$root\tests\dll_load_test.c", '/Fe:dll-load-test.exe', '/link', '/MACHINE:X86'))
        Compile ($flags + $actorSources + $multiplayerSources + $hookSources + @("$root\tests\tick_hook_test.c", "$root\src\diag_tick.c", "$repo\asmp-dll\src\game\probe.c", "$root\src\dummy_actor.c", '/Fe:tick-hook-test.exe', '/link', '/MACHINE:X86'))
        Compile ($flags + $actorSources + @("$root\tests\dummy_actor_test.c", "$root\src\dummy_actor.c", '/Fe:dummy-actor-test.exe', '/link', '/MACHINE:X86'))
        Compile ($flags + $actorSources + $multiplayerSources + $hookSources + @("$root\tests\multiplayer_test.c", "$repo\asmp-dll\src\game\probe.c", '/Fe:multiplayer-test.exe', '/link', '/MACHINE:X86'))
        Compile ($flags + $actorSources + $multiplayerSources + $hookSources + @("$root\tests\session_test.c", "$repo\asmp-dll\src\game\probe.c", '/Fe:session-test.exe', '/link', '/MACHINE:X86'))
        Compile ($flags + $actorSources + $multiplayerSources + $hookSources + $netIncludes + @($runtimeSource, "$root\tests\runtime_test.c", "$repo\asmp-dll\src\game\probe.c", "$repo\asmp-dll\src\multiplayer\client\state_client.c", '/Fe:runtime-test.exe') + $netObjects + @('/link', 'ws2_32.lib', 'advapi32.lib', '/MACHINE:X86'))
        Compile ($flags + $actorSources + $multiplayerSources + $hookSources + @("$root\tests\hooks_test.c", "$repo\asmp-dll\src\game\probe.c", '/Fe:hooks-test.exe', '/link', '/MACHINE:X86'))
        & .\hooks-test.exe
        if ($LASTEXITCODE) { throw 'Hook lifecycle checks failed.' }
        & .\runtime-test.exe
        if ($LASTEXITCODE) { throw 'Runtime checks failed.' }
        & .\session-test.exe
        if ($LASTEXITCODE) { throw 'Session checks failed.' }
        & .\multiplayer-test.exe
        if ($LASTEXITCODE) { throw 'Multiplayer coordinator checks failed.' }
        & .\dummy-actor-test.exe
        if ($LASTEXITCODE) { throw 'Dummy actor lifecycle checks failed.' }
        & .\probe-test.exe
        if ($LASTEXITCODE) { throw 'Probe checks failed.' }
        & .\dll-load-test.exe "$build\asmp-diag.dll"
        if ($LASTEXITCODE) { throw 'DLL loading/rejection check failed.' }
        foreach ($mode in @('normal', 'mismatch', 'invalid', 'foreign', 'concurrent')) {
            & .\tick-hook-test.exe $mode
            if ($LASTEXITCODE) { throw "Tick hook checks failed ($mode)." }
        }
    }
    if ($Server) {
        $repo = Split-Path $root -Parent
        $sourceDirs = @("$repo\asmp-server\src", "$repo\common\src", "$repo\common\epnet\src\server", "$repo\common\epnet\src\common")
        $files = @($sourceDirs | ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -Filter *.c } | ForEach-Object FullName)
        $includeDirs = @($sourceDirs) + @($sourceDirs | ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -Directory } | ForEach-Object FullName) + "$repo\common\epnet\include"
        $includes = @($includeDirs | Select-Object -Unique | ForEach-Object { '/I' + $_ })
        Compile (@('/nologo', '/std:c11', '/W3', '/O2', '/MT', '/DWIN32_LEAN_AND_MEAN') + $includes + $files + @('/Fe:asmp-server.exe', '/link', 'ws2_32.lib', 'advapi32.lib', '/MACHINE:X86'))
    }
} finally { Pop-Location }
Write-Output "Built x86 diagnostics in $build"
