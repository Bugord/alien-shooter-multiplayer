$ErrorActionPreference = 'Stop'
$diagnosticsRoot = Split-Path $PSScriptRoot -Parent
$bashCommand = Get-Command bash.exe -ErrorAction SilentlyContinue
$bash = if ($bashCommand) { $bashCommand.Source } else { Join-Path $env:ProgramFiles 'Git\bin\bash.exe' }
if (!(Test-Path -LiteralPath $bash -PathType Leaf)) {
    Write-Output 'SKIP Bash wrapper checks: Git Bash is not installed.'
    return
}
function Check($Condition, [string]$Message) { if (!$Condition) { throw "CHECK failed: $Message" } }
$fixtureParent = Join-Path $diagnosticsRoot 'build\bash-wrapper-tests'
$fixture = Join-Path $fixtureParent ('path with spaces-' + [guid]::NewGuid().ToString('N'))
$previousCapture = $env:ASMP_BASH_TEST_CAPTURE
$previousExit = $env:ASMP_BASH_TEST_EXIT
try {
    New-Item -ItemType Directory -Path $fixture | Out-Null
    foreach ($name in @('bash-common.sh', 'start-server.sh', 'start-clients.sh')) {
        $path = Join-Path $diagnosticsRoot $name
        & $bash --noprofile --norc -n $path.Replace('\', '/')
        Check ($LASTEXITCODE -eq 0) "Bash parser: $name"
        Copy-Item -LiteralPath $path -Destination (Join-Path $fixture $name)
    }
    $capture = Join-Path $fixture 'arguments.json'
    $env:ASMP_BASH_TEST_CAPTURE = $capture
    $env:ASMP_BASH_TEST_EXIT = '0'
    $stub = @'
param([string]$ServerAddress, [int]$Port = 27020, [string]$NameA, [string]$NameB)
[pscustomobject]@{ Script = $PSCommandPath; Server = $ServerAddress; Port = $Port; NameA = $NameA; NameB = $NameB } |
    ConvertTo-Json | Set-Content -LiteralPath $env:ASMP_BASH_TEST_CAPTURE -Encoding utf8
exit ([int]$env:ASMP_BASH_TEST_EXIT)
'@
    foreach ($name in @('start-server.ps1', 'start-clients.ps1')) { Set-Content -LiteralPath (Join-Path $fixture $name) -Value $stub -Encoding ascii }
    $runner = Join-Path $fixture 'run-outside-repo.sh'
    [IO.File]::WriteAllText($runner, ('cd /' + "`n" + 'exec bash "$@"' + "`n"), [Text.Encoding]::ASCII)
    function Invoke-Wrapper([string]$Name, [string[]]$Arguments, [int]$ExpectedExit = 0) {
        $wrapper = (Join-Path $fixture $Name).Replace('\', '/')
        # A fixed runner avoids PowerShell 5.1 stripping quotes from bash -c code.
        & $bash --noprofile --norc $runner.Replace('\', '/') $wrapper @Arguments
        Check ($LASTEXITCODE -eq $ExpectedExit) "$Name exit status ($ExpectedExit)"
    }
    Invoke-Wrapper 'start-server.sh' @()
    $result = Get-Content -LiteralPath $capture -Raw | ConvertFrom-Json
    Check ($result.Port -eq 27020 -and $result.Script -eq (Join-Path $fixture 'start-server.ps1')) 'default server and script path with spaces'
    Invoke-Wrapper 'start-clients.sh' @()
    $result = Get-Content -LiteralPath $capture -Raw | ConvertFrom-Json
    Check ($result.Server -eq '127.0.0.1' -and $result.Port -eq 27020 -and $result.NameA -eq 'LocalA' -and $result.NameB -eq 'LocalB') 'default client arguments'
    Invoke-Wrapper 'start-clients.sh' @('192.168.1.10', '27021', "A O'Neil", 'B;$x')
    $result = Get-Content -LiteralPath $capture -Raw | ConvertFrom-Json
    Check ($result.Server -eq '192.168.1.10' -and $result.Port -eq 27021 -and $result.NameA -eq "A O'Neil" -and $result.NameB -eq 'B;$x') 'custom arguments survive quoting literally'
    Remove-Item -LiteralPath $capture
    foreach ($port in @('0', '65536', '-1', 'text', '999999999999999999999999')) { Invoke-Wrapper 'start-server.sh' @($port) 2 }
    Invoke-Wrapper 'start-clients.sh' @('127.0.0.1', '0') 2
    Invoke-Wrapper 'start-server.sh' @('27020', 'extra') 2
    Invoke-Wrapper 'start-clients.sh' @('127.0.0.1', '27020', 'A', 'B', 'extra') 2
    Check (!(Test-Path -LiteralPath $capture)) 'invalid arguments never call PowerShell entry point'
    Invoke-Wrapper 'start-server.sh' @('--help')
    Invoke-Wrapper 'start-clients.sh' @('--help')
    Check (!(Test-Path -LiteralPath $capture)) 'help does not launch anything'
    $env:ASMP_BASH_TEST_EXIT = '23'
    Invoke-Wrapper 'start-clients.sh' @() 23
    Write-Output 'Bash wrapper checks passed.'
} finally {
    $env:ASMP_BASH_TEST_CAPTURE = $previousCapture
    $env:ASMP_BASH_TEST_EXIT = $previousExit
    if (Test-Path -LiteralPath $fixture) {
        $resolvedFixture = (Resolve-Path -LiteralPath $fixture).Path
        $resolvedParent = (Resolve-Path -LiteralPath $fixtureParent).Path
        if ([IO.Path]::GetDirectoryName($resolvedFixture) -ine $resolvedParent) { throw 'Unexpected Bash fixture cleanup path.' }
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
