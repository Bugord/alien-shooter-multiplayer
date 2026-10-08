$ErrorActionPreference = 'Stop'
$diagnosticsRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $diagnosticsRoot 'test-session.ps1')
function Check($Condition, [string]$Message) { if (!$Condition) { throw "CHECK failed: $Message" } }
function Expect-Throw([scriptblock]$Action, [string]$Message) {
    $thrown = $false
    try { & $Action | Out-Null } catch { $thrown = $true }
    Check $thrown $Message
}
foreach ($file in @(Get-ChildItem -LiteralPath $diagnosticsRoot -Filter *.ps1) + @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter *.ps1)) {
    $tokens = $null; $parseErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$parseErrors)
    Check ($parseErrors.Count -eq 0) "PowerShell parser: $($file.Name)"
}
$defaultDirectory = Get-TestGameDirectory
$aDirectory = Get-TestGameDirectory 'client-a'
$bDirectory = Get-TestGameDirectory 'client-b'
Check ($defaultDirectory -eq (Join-Path $diagnosticsRoot 'test-game')) 'default path preserved'
Check ($aDirectory -ne $bDirectory -and $aDirectory -ne $defaultDirectory) 'separate game paths'
Expect-Throw { Get-TestGameDirectory 'unknown' } 'unknown instance refused'
Expect-Throw { Assert-TestDirectory (Join-Path $diagnosticsRoot '..\asmp-dll') $diagnosticsRoot } 'parent traversal refused'
Expect-Throw { Assert-TestDirectory ($diagnosticsRoot + '-other\game') $diagnosticsRoot } 'sibling prefix refused'
Check ((Assert-TestDirectory $aDirectory $diagnosticsRoot) -eq $aDirectory) 'valid child path'

# Mock enumeration, preserving any caller's Get-Process override.
$previousGetProcess = Get-Item Function:\Get-Process -ErrorAction SilentlyContinue
$script:fakeProcesses = @()
function Get-Process { param($Name, $ErrorAction) $script:fakeProcesses }
try {
    Assert-TestGameIdle $aDirectory
    $script:fakeProcesses = @([pscustomobject]@{Id=1; Path=(Join-Path $aDirectory 'AlienShooter.exe')})
    Expect-Throw { Assert-TestGameIdle $aDirectory } 'running selected copy refused'
    $script:fakeProcesses = @([pscustomobject]@{Id=2; Path=(Join-Path $bDirectory 'AlienShooter.exe')})
    Assert-TestGameIdle $aDirectory -AllowedOtherGame (Join-Path $bDirectory 'AlienShooter.exe')
    Expect-Throw { Assert-TestGameIdle $defaultDirectory -AllGames } 'default launch refuses another game'
    $script:fakeProcesses = @([pscustomobject]@{Id=3; Path='D:\Steam\AlienShooter.exe'})
    Expect-Throw { Assert-TestGameIdle $aDirectory -AllowedOtherGame (Join-Path $bDirectory 'AlienShooter.exe') } 'foreign game refused for pair launch'
    $script:fakeProcesses = @([pscustomobject]@{Id=4; Path=$null})
    Expect-Throw { Assert-TestGameIdle $aDirectory } 'unknown path refused'
} finally {
    Remove-Item Function:\Get-Process
    if ($previousGetProcess) { Set-Item Function:\Get-Process $previousGetProcess.ScriptBlock }
}

# Exclusive lock protects preparations and launch settings against races.
$lock = Enter-TestSessionLock
try { Expect-Throw { Enter-TestSessionLock } 'concurrent script refused' }
finally { $lock.Dispose() }
$lock = Enter-TestSessionLock
$lock.Dispose()
Write-Output 'Test instance script checks passed.'
