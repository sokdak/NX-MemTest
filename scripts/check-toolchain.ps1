$ErrorActionPreference = "Stop"

function Test-Command($Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        Write-Host "missing: $Name"
        return $false
    }
    Write-Host "found:   $Name -> $($cmd.Source)"
    return $true
}

$ok = $true
$ok = (Test-Command "cmake") -and $ok
$ok = ((Test-Command "gcc") -or (Test-Command "clang") -or (Test-Command "cl")) -and $ok
$ok = (Test-Command "make") -and $ok
$ok = (Test-Command "aarch64-none-elf-gcc") -and $ok
$ok = (Test-Command "nxlink") -and $ok

if (-not $env:DEVKITPRO) {
    Write-Host "missing: DEVKITPRO environment variable"
    $ok = $false
} else {
    Write-Host "found:   DEVKITPRO -> $env:DEVKITPRO"
}

if (-not $ok) {
    exit 1
}
exit 0
