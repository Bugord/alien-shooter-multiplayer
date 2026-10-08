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
Push-Location $build
try {
    Compile ($flags + @('/LD', "$root\src\diag.c", "$root\src\probe.c", "$root\src\hash.c", '/Fe:asmp-steam-diag.dll', '/link', 'bcrypt.lib', '/MACHINE:X86', '/INCREMENTAL:NO'))
    Compile ($flags + @("$root\src\launcher.c", "$root\src\hash.c", '/Fe:asmp-diag-launch.exe', '/link', 'bcrypt.lib', 'user32.lib', '/MACHINE:X86', '/INCREMENTAL:NO'))
    if (!$SkipTests) {
        Compile ($flags + @("$root\tests\probe_test.c", "$root\src\probe.c", '/Fe:probe-test.exe', '/link', '/MACHINE:X86'))
        Compile ($flags + @("$root\tests\dll_load_test.c", '/Fe:dll-load-test.exe', '/link', '/MACHINE:X86'))
        & .\probe-test.exe
        if ($LASTEXITCODE) { throw 'Probe checks failed.' }
        & .\dll-load-test.exe "$build\asmp-steam-diag.dll"
        if ($LASTEXITCODE) { throw 'DLL loading/rejection check failed.' }
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
