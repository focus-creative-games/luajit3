# Compare LuaTier vs reference PUC-Rio Lua on bench/micro.
# Primary metric: os.clock() reported by each script as time=<seconds>.
# Usage:
#   .\scripts\run_bench.ps1
#   .\scripts\run_bench.ps1 -RefLua path\to\lua53.exe -Luatier path\to\luatier.exe -Runs 3

param(
  [string]$RefLua = $env:LUATIER_REF_LUA,
  [string]$Luatier = $env:LUATIER_BIN,
  [int]$Runs = 3,
  [string]$Report = "",
  [string]$BenchDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $RefLua) {
  $RefLua = "D:\workspace\zlua\SrcRepo\lua-5.3.6\lua53.exe"
}
if (-not (Test-Path $RefLua)) {
  throw "Reference Lua not found: $RefLua (set -RefLua or LUATIER_REF_LUA)"
}

if (-not $Luatier) {
  $candidates = @(
    (Join-Path $Root "build\Release\luatier.exe"),
    (Join-Path $Root "build\RelWithDebInfo\luatier.exe"),
    (Join-Path $Root "build\Debug\luatier.exe"),
    (Join-Path $Root "build\luatier.exe")
  )
  $Luatier = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Luatier -or -not (Test-Path $Luatier)) {
  throw "luatier binary not found; build Release first (cmake --build build --config Release --target luatier_cli)"
}

if (-not $BenchDir) {
  $BenchDir = Join-Path $Root "bench\micro"
}
if (-not $Report) {
  $Report = Join-Path $Root "docs\bench-report.md"
}

$scripts = @(
  "loop_arith.lua",
  "loop_float.lua",
  "table_array.lua",
  "table_hash.lua",
  "funcall.lua",
  "closure.lua",
  "string_ops.lua",
  "pattern.lua",
  "coroutine_switch.lua",
  "binary_trees.lua",
  "fib_rec.lua",
  "table_sort.lua",
  "metatable.lua"
)

function Get-VersionLine([string]$exe) {
  $out = & $exe -v 2>&1 | Out-String
  $line = ($out -split "`r?`n" | Where-Object { $_.Trim() -ne "" } | Select-Object -First 1)
  if (-not $line) {
    $out = & $exe --version 2>&1 | Out-String
    $line = ($out -split "`r?`n" | Where-Object { $_.Trim() -ne "" } | Select-Object -First 1)
  }
  return $line.Trim()
}

function Parse-OkLine([string]$line) {
  if ($line -notmatch '(?m)^ok\b') {
    return $null
  }
  $sec = $null
  if ($line -match 'time=([0-9]+(?:\.[0-9]+)?)') {
    $sec = [double]$Matches[1]
  }
  $payload = ($line -replace '\s*time=[0-9.]+', '').Trim()
  return [pscustomobject]@{ Sec = $sec; Payload = $payload; Raw = $line.Trim() }
}

function Measure-One([string]$exe, [string]$scriptPath) {
  $outFile = Join-Path $env:TEMP ("luatier_bench_out_{0}.txt" -f [guid]::NewGuid().ToString("N"))
  $errFile = Join-Path $env:TEMP ("luatier_bench_err_{0}.txt" -f [guid]::NewGuid().ToString("N"))
  try {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList @($scriptPath) `
      -WorkingDirectory (Split-Path -Parent $scriptPath) `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput $outFile `
      -RedirectStandardError $errFile
    $sw.Stop()
    $stdout = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
    $stderr = Get-Content $errFile -Raw -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) {
      throw ("FAILED exit={0} exe={1} script={2}`nstdout:`n{3}`nstderr:`n{4}" -f `
        $p.ExitCode, $exe, $scriptPath, $stdout, $stderr)
    }
    $line = ($stdout -split "`r?`n" | Where-Object { $_.Trim() -ne "" } | Select-Object -First 1)
    $parsed = Parse-OkLine $line
    if (-not $parsed) {
      throw ("FAILED missing ok: exe={0} script={1}`nstdout:`n{2}`nstderr:`n{3}" -f `
        $exe, $scriptPath, $stdout, $stderr)
    }
    if ($null -eq $parsed.Sec) {
      throw ("FAILED missing time=: exe={0} script={1} line={2}" -f $exe, $scriptPath, $line)
    }
    return [pscustomobject]@{
      Sec = $parsed.Sec
      WallMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 1)
      Payload = $parsed.Payload
      Raw = $parsed.Raw
    }
  } finally {
    Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
  }
}

function Measure-Best([string]$exe, [string]$scriptPath, [int]$n) {
  [void](Measure-One $exe $scriptPath) # warmup
  $times = @()
  $walls = @()
  $last = $null
  for ($i = 0; $i -lt $n; $i++) {
    $r = Measure-One $exe $scriptPath
    $times += $r.Sec
    $walls += $r.WallMs
    $last = $r
  }
  $best = ($times | Measure-Object -Minimum).Minimum
  $avg = [math]::Round((($times | Measure-Object -Average).Average), 6)
  return [pscustomobject]@{
    BestSec = [math]::Round($best, 6)
    AvgSec = $avg
    Samples = (($times | ForEach-Object { "{0:N3}" -f $_ }) -join ", ")
    WallBestMs = ($walls | Measure-Object -Minimum).Minimum
    Payload = $last.Payload
    Raw = $last.Raw
  }
}

$refVer = Get-VersionLine $RefLua
$ltVer = Get-VersionLine $Luatier
$hostInfo = [System.Environment]::OSVersion.VersionString
$when = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name

Write-Host "Reference : $RefLua"
Write-Host "            $refVer"
Write-Host "LuaTier   : $Luatier"
Write-Host "            $ltVer"
Write-Host "Runs/best : $Runs (plus 1 warmup); metric=os.clock()"
Write-Host ""

$results = @()
foreach ($name in $scripts) {
  $path = Join-Path $BenchDir $name
  if (-not (Test-Path $path)) {
    Write-Warning "skip missing $name"
    continue
  }
  Write-Host "=== $name ==="
  $ref = Measure-Best $RefLua $path $Runs
  Write-Host ("  lua53   best={0:N3}s  avg={1:N3}s  [{2}]  {3}" -f $ref.BestSec, $ref.AvgSec, $ref.Samples, $ref.Payload)
  $lt = Measure-Best $Luatier $path $Runs
  Write-Host ("  luatier best={0:N3}s  avg={1:N3}s  [{2}]  {3}" -f $lt.BestSec, $lt.AvgSec, $lt.Samples, $lt.Payload)
  $ratio = if ($ref.BestSec -gt 0) { [math]::Round($lt.BestSec / $ref.BestSec, 2) } else { [double]::NaN }
  $match = ($ref.Payload -eq $lt.Payload)
  if (-not $match) {
    Write-Warning ("output mismatch: ref='{0}' lt='{1}'" -f $ref.Payload, $lt.Payload)
  }
  $results += [pscustomobject]@{
    Name = $name
    RefBest = $ref.BestSec
    RefAvg = $ref.AvgSec
    LtBest = $lt.BestSec
    LtAvg = $lt.AvgSec
    Ratio = $ratio
    Match = $match
    RefOut = $ref.Payload
    LtOut = $lt.Payload
  }
}

$geomean = 1.0
$count = 0
foreach ($r in $results) {
  if ($r.Ratio -gt 0) {
    $geomean *= $r.Ratio
    $count++
  }
}
if ($count -gt 0) {
  $geomean = [math]::Round([math]::Exp([math]::Log($geomean) / $count), 2)
} else {
  $geomean = [double]::NaN
}

$faster = @($results | Where-Object { $_.Ratio -lt 1 }).Count
$slower = @($results | Where-Object { $_.Ratio -gt 1 }).Count
$tied = @($results | Where-Object { $_.Ratio -eq 1 }).Count
$mismatches = @($results | Where-Object { -not $_.Match }).Count

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("# LuaTier vs Lua 5.3.6 Performance Report")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Generated: **$when**")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Environment")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Item | Value |")
[void]$sb.AppendLine("|------|-------|")
[void]$sb.AppendLine("| Host | $hostInfo |")
[void]$sb.AppendLine("| CPU | $cpu |")
[void]$sb.AppendLine("| Reference | ``$RefLua`` |")
[void]$sb.AppendLine("| Reference version | $refVer |")
[void]$sb.AppendLine("| LuaTier | ``$Luatier`` |")
[void]$sb.AppendLine("| LuaTier version | $ltVer |")
[void]$sb.AppendLine("| Build | Release ``luatier.exe`` |")
[void]$sb.AppendLine("| Methodology | in-script ``os.clock()``; 1 warmup + $Runs timed runs; report **best** |")
[void]$sb.AppendLine("| Suite | ``bench/micro`` (13 microbenchmarks) |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Summary")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- Geometric mean of (LuaTier / Lua 5.3.6) best-time ratios: **${geomean}x**")
[void]$sb.AppendLine("- Ratio > 1 means LuaTier is slower; < 1 means faster.")
[void]$sb.AppendLine("- Counts: faster=$faster, slower=$slower, tied=$tied; result mismatches=$mismatches")
[void]$sb.AppendLine("- ``Match`` compares the ``ok ...`` payload with the ``time=`` field stripped.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Results")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Benchmark | Lua 5.3.6 best (s) | LuaTier best (s) | Ratio (LT/PUC) | Match |")
[void]$sb.AppendLine("|-----------|-------------------:|-----------------:|---------------:|:-----:|")
foreach ($r in $results) {
  $m = if ($r.Match) { "yes" } else { "NO" }
  [void]$sb.AppendLine(("| ``{0}`` | {1:N3} | {2:N3} | {3}x | {4} |" -f `
    $r.Name, $r.RefBest, $r.LtBest, $r.Ratio, $m))
}
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Per-benchmark detail")
[void]$sb.AppendLine("")
foreach ($r in $results) {
  [void]$sb.AppendLine("### ``$($r.Name)``")
  [void]$sb.AppendLine("")
  [void]$sb.AppendLine("| Runtime | Best (s) | Avg (s) | Output payload |")
  [void]$sb.AppendLine("|---------|---------:|--------:|----------------|")
  [void]$sb.AppendLine("| Lua 5.3.6 | $("{0:N6}" -f $r.RefBest) | $("{0:N6}" -f $r.RefAvg) | ``$($r.RefOut)`` |")
  [void]$sb.AppendLine("| LuaTier | $("{0:N6}" -f $r.LtBest) | $("{0:N6}" -f $r.LtAvg) | ``$($r.LtOut)`` |")
  [void]$sb.AppendLine("")
}
[void]$sb.AppendLine("## Notes")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- LuaTier is currently interpreter-only (no AOT/JIT tier active in these runs).")
[void]$sb.AppendLine("- Prefer a **Release** ``luatier.exe``; Debug builds are not comparable.")
[void]$sb.AppendLine("- Primary metric excludes process startup; wall-clock still includes parse/load.")
[void]$sb.AppendLine("- Re-run: ``.\scripts\run_bench.ps1``")

$md = $sb.ToString()
$dir = Split-Path -Parent $Report
if (-not (Test-Path $dir)) {
  New-Item -ItemType Directory -Path $dir | Out-Null
}
Set-Content -Path $Report -Value $md -Encoding UTF8

Write-Host ""
Write-Host ("Geometric mean ratio (LuaTier/Lua5.3.6): {0}x" -f $geomean)
Write-Host "Report written: $Report"
