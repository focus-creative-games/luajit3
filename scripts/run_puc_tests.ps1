$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Suite = Join-Path $Root "tests\lua-5.3.4-tests"
$candidates = @(
  (Join-Path $Root "build\Debug\luatier.exe"),
  (Join-Path $Root "build\Release\luatier.exe"),
  (Join-Path $Root "build\luatier.exe")
)
$Lua = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Lua) { throw "luatier binary not found; build first" }
Push-Location $Suite
try {
  $out = & $Lua -e"_U=true" all.lua 2>&1 | Out-String
  Write-Host $out
  if ($out -notmatch "final OK") {
    throw "PUC basic suite failed: missing 'final OK'"
  }
} finally {
  Pop-Location
}
