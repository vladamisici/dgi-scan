<#
.SYNOPSIS
  Turns Scantailor-DGI diagnostics logs into report.html and summary.json.

.DESCRIPTION
  Reads every scantailor-perf-*.jsonl under -Path (a stress-test output folder,
  a folder or zip made by Collect-Diagnostics, a crash-log folder or a single
  .jsonl file) and writes, into -Out:
    report.html   self-contained report (no external files, opens offline)
    summary.json  the key numbers, for Compare-Reports.ps1
    stalls.csv, ops.csv  raw tables for further digging

  The log format is described in SCHEMA.md next to this script.

.PARAMETER Path
  Folder, .zip or .jsonl to analyse.

.PARAMETER Out
  Output folder. Default: <Path>\report (for a zip: <zip name>-report next to it).
  Also written there: summary.md, the verdicts in Markdown (for a CI job summary).

.PARAMETER SampleCap
  Per operation, at most this many durations are kept for percentiles; beyond
  it a uniform random sample is kept (counts, totals and maxima stay exact).

.PARAMETER Open
  Open report.html in the default browser when done.

.EXAMPLE
  .\Analyze-Diagnostics.ps1 -Path C:\Users\ana\AppData\Local\scantailor-dgi-stress\20260923-101500

.NOTES
  Exit code: 0 = no hard failure; 1 = a hard failure (crash, bad exit code,
  stress instance that could not start or wrote no log, task failure,
  unexpected dialog, failed page load) or no logs at all. An instance the
  runner killed because the run was aborted is a WARN, not a crash.
  Timing and memory verdicts are in the report but never change the exit code:
  on a shared or busy machine they are too noisy to gate on.
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0)][Alias('RunDir')][string]$Path,
  [Alias('OutDir')][string]$Out,
  [int]$SampleCap = 50000,
  [int]$MaxStallRows = 200,
  [switch]$Open,
  [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
try {
  Get-ChildItem -LiteralPath $PSScriptRoot -File -ErrorAction Stop |
    Where-Object { $_.Extension -match '^\.(ps1|psm1|cmd)$' } | Unblock-File -ErrorAction SilentlyContinue
} catch { }
Import-Module (Join-Path $PSScriptRoot 'DiagnosticsCommon.psm1') -DisableNameChecking

$Inv = [System.Globalization.CultureInfo]::InvariantCulture

# Verdict thresholds. Kept together so that a reader of the report (and of this
# script) can see exactly what "WARN" and "FAIL" mean.
$Thresholds = @{
  StallWarnMs        = 1000   # any stall longer than this -> WARN
  StallFailMs        = 5000   # any stall longer than this -> FAIL
  StalledPctWarn     = 1.0    # total stalled time above this % of the run -> WARN
  MinLeakCycles      = 4      # cycle_end samples needed after the warm-up cycle
  LeakMetrics        = @(
    @{ key = 'private_mb'; label = 'Private memory (commit)'; unit = 'MB'; warn = 2; fail = 10 },
    @{ key = 'handles'; label = 'Kernel handles'; unit = 'handles'; warn = 20; fail = 100 },
    @{ key = 'gdi'; label = 'GDI objects'; unit = 'objects'; warn = 10; fail = 50 },
    @{ key = 'user'; label = 'USER objects'; unit = 'objects'; warn = 10; fail = 50 },
    @{ key = 'threads'; label = 'Threads'; unit = 'threads'; warn = 2; fail = 10 }
  )
  LeakWarnR2         = 0.6
  LeakFailR2         = 0.8
  GuiSlowOpMs        = 250    # GUI-thread operations at least this long are highlighted
}

# stress.autosave branches that write nothing by design (SCHEMA.md).
$AutosaveSkipBranches = @('batch_skip', 'guard_skip', 'disabled', 'busy')

$StageOps = @('stage.fix_orientation', 'stage.page_split', 'stage.deskew', 'stage.select_content', 'stage.page_layout', 'stage.output')
$StageNames = @('Fix orientation', 'Split pages', 'Deskew', 'Select content', 'Margins', 'Output')
$StageIndex = @{}
for ($i = 0; $i -lt $StageOps.Count; $i++) { $StageIndex[$StageOps[$i]] = $i }
$Palette = Get-DiagPalette

function Write-Info([string]$m) { if (-not $Quiet) { Write-DiagLog $m } }

if (-not $Path) {
  Write-Host 'Usage: Analyze-Diagnostics.ps1 -Path <stress output folder | diagnostics folder | .zip | .jsonl> [-Out <folder>] [-Open]'
  exit 1
}
if (-not (Test-Path -LiteralPath $Path)) {
  Write-DiagLog ('Not found: ' + $Path) 'error'
  exit 1
}
$Path = (Resolve-Path -LiteralPath $Path).ProviderPath
$root = $Path
$singleFile = $null
$tempExtract = $null
if (Test-Path -LiteralPath $Path -PathType Leaf) {
  $item = Get-Item -LiteralPath $Path
  if ($item.Extension -eq '.zip') {
    $tempExtract = Join-Path (Get-DiagTempRoot) ('st-diag-analyze-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    [void][System.IO.Directory]::CreateDirectory($tempExtract)
    Write-Info ('Extracting ' + $item.Name + ' ...')
    try {
      Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
      [System.IO.Compression.ZipFile]::ExtractToDirectory($Path, $tempExtract)
    } catch {
      Expand-Archive -LiteralPath $Path -DestinationPath $tempExtract -Force
    }
    $root = $tempExtract
    # Collect-Diagnostics zips its folder, so run.json/inventory.json sit one level down.
    $top = @(Get-ChildItem -LiteralPath $tempExtract -Force)
    if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $root = $top[0].FullName }
    if (-not $Out) { $Out = Join-Path $item.DirectoryName ($item.BaseName + '-report') }
  } else {
    $singleFile = $item
    $root = $item.DirectoryName
    if (-not $Out) { $Out = Join-Path $item.DirectoryName ('report-' + $item.BaseName) }
  }
}
if (-not $Out) { $Out = Join-Path $root 'report' }
# A relative -Out is relative to PowerShell's current location; .NET would
# resolve it against the process directory, which can be somewhere else.
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
[void][System.IO.Directory]::CreateDirectory($Out)
$Out = (Resolve-Path -LiteralPath $Out).ProviderPath

# ---------------------------------------------------------------------------
# Discover files
# ---------------------------------------------------------------------------

$perfRx = New-Object System.Text.RegularExpressions.Regex('^scantailor-perf-(\d{8}-\d{6})-(\d+)(?:\.(\d+))?\.jsonl$', 'IgnoreCase')
$crashRx = New-Object System.Text.RegularExpressions.Regex('^scantailor-\d{8}-\d{6}-\d+\.(dmp|txt)$', 'IgnoreCase')
$perfFiles = @()
$crashFiles = @()
$logFiles = @()
# The output folder is skipped only when it lies strictly inside the folder
# being analysed (the default <Path>\report). Compared with a trailing
# separator, so that -Out C:\logs-report does not hide C:\logs-reports\...,
# and never when -Out is the analysed folder itself or one of its parents.
# Both are spelled the way Get-ChildItem spells what it lists: Windows
# PowerShell expands 8.3 short names (C:\Users\ANA~1\...) in listings, so a
# string comparison with the path as given would never match.
function Get-ListedSpelling([string]$dir) {
  $c = Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($null -eq $c) { return $dir }
  if ($c.PSIsContainer) { return $c.Parent.FullName }
  return $c.DirectoryName
}
$outSkip = $null
$rootListed = Get-ListedSpelling $root
$rootSep = $rootListed.TrimEnd('\', '/') + '\'
$outSep = (Get-ListedSpelling $Out).TrimEnd('\', '/') + '\'
if ($outSep.Length -gt $rootSep.Length -and $outSep.StartsWith($rootSep, [System.StringComparison]::OrdinalIgnoreCase)) { $outSkip = $outSep }
if ($singleFile) {
  $perfFiles = @($singleFile)
} else {
  foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter 'scantailor*' -ErrorAction SilentlyContinue)) {
    if ($outSkip -and $f.FullName.StartsWith($outSkip, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    if ($perfRx.IsMatch($f.Name)) { $perfFiles += $f }
    elseif ($crashRx.IsMatch($f.Name)) { $crashFiles += $f }
    elseif ($f.Name -like 'scantailor.log*') { $logFiles += $f }
  }
}

function Read-OptionalJson([string]$p) {
  if (Test-Path -LiteralPath $p -PathType Leaf) {
    try { return (Read-DiagJson -Path $p) } catch { Write-DiagLog ('Could not read ' + $p + ': ' + $_.Exception.Message) 'warn' }
  }
  return $null
}
$runInfo = Read-OptionalJson (Join-Path $root 'run.json')
$inventory = $null
if ($runInfo -and $runInfo.inventory) { $inventory = $runInfo.inventory }
$invFile = Read-OptionalJson (Join-Path $root 'inventory.json')
if ($invFile) { $inventory = $invFile }
$storage = @()
if ($runInfo -and $runInfo.storage) { $storage = @($runInfo.storage) }
$stFile = Read-OptionalJson (Join-Path $root 'storage.json')
if ($stFile) { $storage = @($stFile) }
$manifest = Read-OptionalJson (Join-Path $root 'manifest.json')
$label = ''
if ($runInfo -and $runInfo.label) { $label = [string]$runInfo.label }
elseif ($manifest -and $manifest.label) { $label = [string]$manifest.label }

$groups = @{}
foreach ($f in $perfFiles) {
  $m = $perfRx.Match($f.Name)
  $stamp = ''; $procId = ''; $part = 1
  if ($m.Success) {
    $stamp = $m.Groups[1].Value; $procId = $m.Groups[2].Value
    if ($m.Groups[3].Success) { $part = [int]$m.Groups[3].Value }
  } else {
    $stamp = $f.BaseName; $procId = '0'
  }
  $key = $f.DirectoryName + '|' + $stamp + '|' + $procId
  if (-not $groups.ContainsKey($key)) {
    $inst = $null
    $dirLeaf = Split-Path -Leaf $f.DirectoryName
    $parentLeaf = Split-Path -Leaf (Split-Path -Parent $f.DirectoryName)
    if ($dirLeaf -eq 'logs' -and $parentLeaf -match '^inst(\d+)$') { $inst = [int]$Matches[1] }
    $groups[$key] = @{ key = $key; stamp = $stamp; pid = $procId; dir = $f.DirectoryName; instance = $inst; files = @() }
  }
  $groups[$key].files += , @{ file = $f; part = $part }
}
$groupList = @($groups.Values | Sort-Object { $_.stamp }, { [int]$_.pid })
if ($groupList.Count -eq 0) {
  Write-DiagLog ('No scantailor-perf-*.jsonl files under ' + $root) 'warn'
}
$totalBytes = 0
foreach ($f in $perfFiles) { $totalBytes += $f.Length }
Write-Info ('Found ' + $perfFiles.Count + ' log file(s) in ' + $groupList.Count + ' process(es), ' + (Format-DiagBytes $totalBytes) + '.')

# ---------------------------------------------------------------------------
# Stream the records. This loop is the hot path: it runs once per log line,
# so it stays inline (a function call per record would double the run time
# in Windows PowerShell) and keeps only aggregates plus the rare records.
# ---------------------------------------------------------------------------

# Durations are kept per "name|thread" in a plain list: exact statistics come
# from it at the end. Past $cap values the list becomes a uniform reservoir
# sample and the exact count/total/max continue in $opOver, so memory stays
# bounded however long the log is.
$opDur = @{}       # "name|thread" -> List[double]
$opOver = @{}      # "name|thread" -> n, sum, max once the list is full
$opName = @{}      # "name|thread" -> @(name, thread)
$aggStats = @{}    # name -> n, sum, max
$stageDur = @{}    # stage op -> List[double] of self times
$stageOver = @{}
$rng = New-Object System.Random(7)
$cap = [Math]::Max(1000, $SampleCap)
$sampled = $false
$procs = @()
$swAll = [System.Diagnostics.Stopwatch]::StartNew()
$totalRecords = [long]0

function New-OverflowState($list) {
  # Exact count, total and maximum of a list that is about to become a sample:
  # @(n, sum, max). Called once per key; the per-record update stays inline
  # because a function call per record costs more than everything else.
  $s0 = 0.0; $m0 = 0.0
  foreach ($v in $list) { $s0 += $v; if ($v -gt $m0) { $m0 = $v } }
  return , ([double[]]@($list.Count, $s0, $m0))
}

$gi = 0
foreach ($g in $groupList) {
  $gi++
  $proc = @{
    key = $g.key; stamp = $g.stamp; pid = $g.pid; dir = $g.dir; instance = $g.instance
    files = @(); start = $null; screens = @(); settings = $null; stop = $null
    first_t = [double]::MaxValue; last_t = 0.0; records = [long]0; bad = [long]0; truncated = $false
    res = (New-Object System.Collections.Generic.List[object])
    stalls = (New-Object System.Collections.Generic.List[object])
    stress = (New-Object System.Collections.Generic.List[object])
    beat = @{ n = [long]0; late50 = [long]0; late100 = [long]0; late250 = [long]0; late1000 = [long]0; max_late = 0.0 }
    outcomes = @{}; other = @{}; phases = @(); oom = @()
    # stall_progress records, latest per stall ("at"): written while a stall
    # is still going, so the last one survives a process that never recovers.
    progress = @{}; unended = @()
  }
  # Last stage record seen per task. A task's records arrive innermost first
  # (scopes close inside out on one thread), so when a stage record arrives the
  # previous one of the same task is the next-deeper stage, whose duration is
  # subtracted to get this stage's own time (SCHEMA.md).
  $lastStage = @{}
  $res = $proc.res; $stalls = $proc.stalls; $stress = $proc.stress; $beat = $proc.beat; $outcomes = $proc.outcomes
  $firstT = [double]::MaxValue; $lastT = 0.0
  foreach ($part in @($g.files | Sort-Object { $_.part })) {
    $f = $part.file
    Write-Info ('  [' + $gi + '/' + $groupList.Count + '] ' + $f.Name + ' (' + (Format-DiagBytes $f.Length) + ')')
    $reader = Open-JsonlReader -Path $f.FullName
    try {
      while ($true) {
        $batch = Read-JsonlBatch -Reader $reader
        if ($null -eq $batch) { break }
        $bn = $batch.Length
        if ($bn -eq 0) { continue }
        # Records are written in time order (to within a few ms across threads),
        # so the ends of each batch bound the process's time span.
        $t = $batch[0].t
        if ($null -ne $t -and [double]$t -lt $firstT) { $firstT = [double]$t }
        $t = $batch[$bn - 1].t
        if ($null -ne $t -and [double]$t -gt $lastT) { $lastT = [double]$t }
        foreach ($r in $batch) {
          $ev = $r.ev
          if ($ev -eq 'op') {
            $name = $r.name
            $dur = [double]$r.dur
            $key = $name + '|' + $r.th
            $lst = $opDur[$key]
            if ($null -eq $lst) {
              $lst = New-Object System.Collections.Generic.List[double]
              $opDur[$key] = $lst
              $opName[$key] = @($name, [string]$r.th)
            }
            if ($lst.Count -lt $cap) { $lst.Add($dur) }
            else {
              $ov = $opOver[$key]
              if ($null -eq $ov) { $ov = New-OverflowState $lst; $opOver[$key] = $ov; $sampled = $true }
              $ov[0]++
              $ov[1] += $dur
              if ($dur -gt $ov[2]) { $ov[2] = $dur }
              $j = $rng.Next([int]$ov[0])
              if ($j -lt $cap) { $lst[$j] = $dur }
            }
            if ($null -ne $StageIndex[$name]) {
              $tk = $r.task
              if ($tk) {
                $depth = $r.depth
                $self = $dur
                $prev = $lastStage[$tk]
                if ($null -ne $prev -and $prev[0] -gt $depth) {
                  $self = $dur - $prev[1]
                  if ($self -lt 0) { $self = 0.0 }
                }
                $lastStage[$tk] = @($depth, $dur)
                $sl = $stageDur[$name]
                if ($null -eq $sl) { $sl = New-Object System.Collections.Generic.List[double]; $stageDur[$name] = $sl }
                if ($sl.Count -lt $cap) { $sl.Add($self) }
                else {
                  $ov = $stageOver[$name]
                  if ($null -eq $ov) { $ov = New-OverflowState $sl; $stageOver[$name] = $ov; $sampled = $true }
                  $ov[0]++
                  $ov[1] += $self
                  if ($self -gt $ov[2]) { $ov[2] = $self }
                  $j = $rng.Next([int]$ov[0])
                  if ($j -lt $cap) { $sl[$j] = $self }
                }
              }
            } elseif ($name -eq 'task.run') {
              $oc = [string]$r.outcome
              if ($outcomes.ContainsKey($oc)) { $outcomes[$oc]++ } else { $outcomes[$oc] = 1 }
              $tk = $r.task
              if ($tk) { $lastStage.Remove($tk) }
            }
          } elseif ($ev -eq 'res') {
            $res.Add($r)
          } elseif ($ev -eq 'agg') {
            $name = $r.name
            $a = $aggStats[$name]
            if ($null -eq $a) { $a = @{ n = [long]0; sum = 0.0; max = 0.0 }; $aggStats[$name] = $a }
            $a.n += [long]$r.n
            $a.sum += [double]$r.sum
            $mx = [double]$r.max
            if ($mx -gt $a.max) { $a.max = $mx }
          } elseif ($ev -eq 'beat') {
            $beat.n += [long]$r.n
            $beat.late50 += [long]$r.late50
            $beat.late100 += [long]$r.late100
            $beat.late250 += [long]$r.late250
            $beat.late1000 += [long]$r.late1000
            $ml = [double]$r.max_late
            if ($ml -gt $beat.max_late) { $beat.max_late = $ml }
          } elseif ($ev -eq 'stall') {
            $stalls.Add($r)
          } elseif ($ev -eq 'stall_progress') {
            $ak = ([double]$r.at).ToString('0.###', $Inv)
            $pp0 = $proc.progress[$ak]
            if ($null -eq $pp0 -or [double]$r.dur -ge [double]$pp0.dur) { $proc.progress[$ak] = $r }
          } elseif ($ev -eq 'start') {
            if ($null -eq $proc.start) { $proc.start = $r }
          } elseif ($ev -eq 'screen') {
            $proc.screens += , $r
          } elseif ($ev -eq 'settings') {
            $proc.settings = $r
          } elseif ($ev -eq 'stop') {
            $proc.stop = $r
          } elseif ($ev -eq 'phase') {
            $proc.phases += , $r
          } elseif ($ev -eq 'oom') {
            $proc.oom += , $r
          } elseif ($ev -like 'stress.*') {
            $stress.Add($r)
          } else {
            $k2 = [string]$ev
            if ($proc.other.ContainsKey($k2)) { $proc.other[$k2]++ } else { $proc.other[$k2] = 1 }
          }
        }
      }
    } finally {
      Close-JsonlReader $reader
    }
    $proc.files += , @{ path = $f.FullName; name = $f.Name; part = $part.part; bytes = $f.Length; records = $reader.Records; bad = $reader.BadLines; truncated = $reader.Truncated; slow_chunks = $reader.SlowChunks }
    $proc.records += $reader.Records
    $proc.bad += $reader.BadLines
    if ($reader.Truncated) { $proc.truncated = $true }
  }
  if ($firstT -eq [double]::MaxValue) { $firstT = 0.0 }
  $proc.first_t = $firstT
  $proc.last_t = $lastT
  # A stall that ends is written as a "stall" record with the same "at" as its
  # progress records. A progress record without one is a stall the log never
  # saw end: it lasted at least the "dur" of its last progress record, and it
  # is kept as a stall of that length (flagged, and never excluded).
  if ($proc.progress.Count -gt 0) {
    $endedAt = @{}
    foreach ($s in $stalls) { if ($null -ne $s.at) { $endedAt[([double]$s.at).ToString('0.###', $Inv)] = $true } }
    foreach ($ak in @($proc.progress.Keys | Sort-Object { [double]$_ })) {
      if ($endedAt.ContainsKey($ak)) { continue }
      $pr = $proc.progress[$ak]
      $syn = @{
        t = $pr.t; ev = 'stall'; th = $pr.th; at = $pr.at; dur = [double]$pr.dur; phase = $pr.phase
        ops = $pr.ops; ops_last = $pr.ops_last; stack = $pr.stack; dump = $pr.dump
        ongoing = $true; unended = $true; log_ends = ($null -eq $proc.stop)
      }
      $stalls.Add($syn)
      $proc.unended += , $syn
    }
  }
  $totalRecords += $proc.records
  $procs += $proc
}
$parseSeconds = $swAll.Elapsed.TotalSeconds
Write-Info ('Parsed ' + $totalRecords + ' records in ' + $parseSeconds.ToString('0.0', $Inv) + ' s.')

# ---------------------------------------------------------------------------
# Per-process derived data
# ---------------------------------------------------------------------------

function ConvertTo-WallStart($wall) {
  if ($null -eq $wall) { return $null }
  if ($wall -is [datetime]) { return [datetimeoffset]$wall }
  $dto = [datetimeoffset]::MinValue
  if ([datetimeoffset]::TryParse([string]$wall, $Inv, [System.Globalization.DateTimeStyles]::None, [ref]$dto)) { return $dto }
  return $null
}

function Get-StallClass($s) {
  # Which stalls count against the application: startup/shutdown stalls do not
  # keep an operator waiting, and stalls inside the test driver's own work
  # (outermost operation stress.*) belong to the test (SCHEMA.md). A stall the
  # log never saw end is always counted: the application was hung, whatever
  # it was doing.
  if ($s.unended) { return 'run' }
  $ph = [string]$s.phase
  if ($ph -eq 'startup' -or $ph -eq 'shutdown') { return $ph }
  $o = @($s.ops | Where-Object { $_ })
  if ($o.Count -eq 0) { $o = @($s.ops_last | Where-Object { $_ }) }
  if ($o.Count -gt 0 -and ([string]$o[0]).StartsWith('stress.', [System.StringComparison]::Ordinal)) { return 'test' }
  return 'run'
}
$StallClassText = @{ run = 'application'; startup = 'startup'; shutdown = 'shutdown'; test = 'test driver' }

function Get-StallInner($s) {
  # Innermost instrumented operation open during a stall, for one-line texts.
  $in = @($s.ops_last | Where-Object { $_ })
  if ($in.Count -eq 0) { $in = @($s.ops | Where-Object { $_ }) }
  if ($in.Count -eq 0) { return '(no instrumented operation open)' }
  return [string]$in[$in.Count - 1]
}

function Get-HangText($s) {
  # "a GUI stall of >= 12.3 s in project.save (phase run)"
  $t = 'a GUI stall of >= ' + (Format-DiagMs $s.dur) + ' in ' + (Get-StallInner $s)
  if ($s.phase) { $t += ' (phase ' + [string]$s.phase + ')' }
  return $t
}

function Format-ExitCode($code) {
  if ($null -eq $code) { return '-' }
  $c = [long]$code
  if ($c -ge 0 -and $c -le 255) { return [string]$c }
  return ([string]$c + ' (0x' + ([int]$c).ToString('X8') + ')')
}

function Get-Num($v) {
  if ($null -eq $v) { return $null }
  try { return [double]$v } catch { return $null }
}

$exitByInstance = @{}
$timedOutByInstance = @{}
if ($runInfo -and $runInfo.instances) {
  foreach ($ri in @($runInfo.instances)) {
    $exitByInstance[[int]$ri.instance] = $ri
  }
}

$isStress = $false
if ($runInfo) { $isStress = $true }
foreach ($p in $procs) { if ($p.stress.Count -gt 0) { $isStress = $true } }

$pi = 0
foreach ($p in $procs) {
  $pi++
  $p.color = $Palette[($pi - 1) % $Palette.Count]
  $p.wall_start = $null
  if ($p.start) { $p.wall_start = ConvertTo-WallStart $p.start.wall }
  $cfg = $null
  foreach ($s in $p.stress) { if ($s.ev -eq 'stress.config') { $cfg = $s; break } }
  $p.config = $cfg
  if ($cfg -and $null -ne $cfg.instance) { $p.instance = [int]$cfg.instance }
  if ($null -ne $p.instance) {
    $p.id = 'inst' + $p.instance
  } else {
    $w = ''
    if ($p.wall_start) { $w = $p.wall_start.ToString('yyyy-MM-dd HH:mm', $Inv) } else { $w = $p.stamp }
    $p.id = $w + ' pid ' + $p.pid
  }
  $p.duration_ms = [Math]::Max(0.0, $p.last_t - $p.first_t)
  # The share of time stalled is taken over the run phase (event loop started
  # until the main window began closing) when the log marks it.
  $runFrom = $p.first_t; $runTo = $p.last_t
  foreach ($ph in $p.phases) {
    if ([string]$ph.phase -eq 'run') { $runFrom = [double]$ph.t }
    elseif ([string]$ph.phase -eq 'shutdown') { $runTo = [double]$ph.t }
  }
  $p.run_ms = [Math]::Max(0.0, $runTo - $runFrom)
  if ($p.run_ms -le 0) { $p.run_ms = $p.duration_ms }
  $p.level = ''
  if ($p.start) { $p.level = [string]$p.start.level }

  # Stalls: those during startup/shutdown or inside the test driver are listed
  # but kept out of the responsiveness verdict.
  $runStalls = @()
  $otherStalls = @()
  $excl = @{ startup = 0; shutdown = 0; test = 0 }
  foreach ($s in $p.stalls) {
    $cls = Get-StallClass $s
    if ($cls -eq 'run') { $runStalls += , $s } else { $otherStalls += , $s; $excl[$cls]++ }
  }
  $total = 0.0; $longest = 0.0; $over1 = 0; $over5 = 0; $unN = 0; $unLongest = 0.0
  foreach ($s in $runStalls) {
    $d = [double]$s.dur
    $total += $d
    if ($d -gt $longest) { $longest = $d }
    if ($s.unended) {
      # At least this long: a lower bound that reaches a threshold counts.
      $unN++
      if ($d -gt $unLongest) { $unLongest = $d }
      if ($d -ge $Thresholds.StallWarnMs) { $over1++ }
      if ($d -ge $Thresholds.StallFailMs) { $over5++ }
      continue
    }
    if ($d -gt $Thresholds.StallWarnMs) { $over1++ }
    if ($d -gt $Thresholds.StallFailMs) { $over5++ }
  }
  $otherLongest = 0.0
  foreach ($s in $otherStalls) { if ([double]$s.dur -gt $otherLongest) { $otherLongest = [double]$s.dur } }
  $pct = 0.0
  if ($p.run_ms -gt 0) { $pct = 100.0 * $total / $p.run_ms }
  $p.stall_stats = [ordered]@{
    count = $runStalls.Count; total_ms = [Math]::Round($total, 1); longest_ms = [Math]::Round($longest, 1)
    over_1s = $over1; over_5s = $over5; pct_of_run = [Math]::Round($pct, 3)
    per_hour = if ($p.run_ms -gt 0) { [Math]::Round($runStalls.Count / ($p.run_ms / 3600000.0), 2) } else { $null }
    # The denominator of pct_of_run and per_hour, kept so that several
    # processes can be combined (Compare-Reports) including stall-free ones.
    run_ms = [Math]::Round($p.run_ms, 1)
    unended = $unN; unended_longest_ms = [Math]::Round($unLongest, 1)
    excluded_count = $otherStalls.Count; excluded_longest_ms = [Math]::Round($otherLongest, 1)
    excluded_startup = $excl.startup; excluded_shutdown = $excl.shutdown; excluded_test = $excl.test
  }

  # Resource series
  $series = @{}
  foreach ($k in @('private_mb', 'ws_mb', 'handles', 'gdi', 'user', 'threads', 'cpu_pct', 'io_read_mb', 'io_write_mb', 'sys_avail_mb', 'sys_load_pct', 'sys_commit_mb')) {
    $series[$k] = @{ X = (New-Object System.Collections.Generic.List[double]); Y = (New-Object System.Collections.Generic.List[double]) }
  }
  $dropped = 0.0; $queueMax = 0.0; $peakWs = 0.0
  foreach ($r in $p.res) {
    $x = [double]$r.t / 60000.0
    foreach ($k in $series.Keys) {
      $v = $r.$k
      if ($null -ne $v) { $series[$k].X.Add($x); $series[$k].Y.Add([double]$v) }
    }
    if ($null -ne $r.log_dropped -and [double]$r.log_dropped -gt $dropped) { $dropped = [double]$r.log_dropped }
    if ($null -ne $r.log_queue -and [double]$r.log_queue -gt $queueMax) { $queueMax = [double]$r.log_queue }
    if ($null -ne $r.peak_ws_mb -and [double]$r.peak_ws_mb -gt $peakWs) { $peakWs = [double]$r.peak_ws_mb }
  }
  if ($p.stop -and $null -ne $p.stop.dropped -and [double]$p.stop.dropped -gt $dropped) { $dropped = [double]$p.stop.dropped }
  $p.series = $series
  $p.log_dropped = $dropped
  $p.log_queue_max = $queueMax
  $p.peak_ws_mb = $peakWs

  # Cycle-end samples (leak detection) and cycle boundaries
  $p.cycle_end = @($p.res | Where-Object { $_.reason -eq 'cycle_end' } | Sort-Object { [int]$_.cycle })
  $p.cycle_begin_t = @($p.stress | Where-Object { $_.ev -eq 'stress.cycle_begin' } | ForEach-Object { [double]$_.t })
  $p.leak = @()
  foreach ($lm in $Thresholds.LeakMetrics) {
    $pts = @($p.cycle_end | Where-Object { $null -ne $_.cycle -and [int]$_.cycle -ge 2 -and $null -ne $_.($lm.key) })
    $entry = [ordered]@{ metric = $lm.key; label = $lm.label; unit = $lm.unit; n = $pts.Count; slope = $null; r2 = $null; first = $null; last = $null; status = 'N/A'; note = '' }
    if ($pts.Count -gt 0) {
      $entry.first = [double]$pts[0].($lm.key)
      $entry.last = [double]$pts[$pts.Count - 1].($lm.key)
    }
    if ($pts.Count -ge $Thresholds.MinLeakCycles) {
      # With enough cycles, judge only the second half. Caches fill during the
      # first 10-20 cycles and then level off, and a line through all of them
      # mistakes that curve for a steady leak; a real leak keeps its slope in the
      # second half too.
      $fitPts = $pts
      $entry.fit_from = [int]$pts[0].cycle
      if ($pts.Count -ge 10) {
        $fitPts = @($pts | Select-Object -Last ([int][Math]::Ceiling($pts.Count / 2)))
        $entry.fit_from = [int]$fitPts[0].cycle
        $whole = Get-LinearFit -X ([double[]]@($pts | ForEach-Object { [double]$_.cycle })) -Y ([double[]]@($pts | ForEach-Object { [double]$_.($lm.key) }))
        if ($whole) { $entry.slope_all = [Math]::Round($whole.slope, 3) }
      }
      $fit = Get-LinearFit -X ([double[]]@($fitPts | ForEach-Object { [double]$_.cycle })) -Y ([double[]]@($fitPts | ForEach-Object { [double]$_.($lm.key) }))
      if ($fit) {
        $entry.slope = [Math]::Round($fit.slope, 3)
        $entry.r2 = [Math]::Round($fit.r2, 3)
        if ($fit.slope -gt $lm.fail -and $fit.r2 -gt $Thresholds.LeakFailR2) { $entry.status = 'FAIL' }
        elseif ($fit.slope -gt $lm.warn -and $fit.r2 -gt $Thresholds.LeakWarnR2) { $entry.status = 'WARN' }
        else { $entry.status = 'PASS' }
      }
    } elseif ($p.cycle_end.Count -gt 0) {
      $entry.note = 'insufficient cycles (' + $pts.Count + ' after the warm-up cycle; ' + $Thresholds.MinLeakCycles + ' needed)'
    } else {
      $entry.note = 'no cycle_end samples'
    }
    $p.leak += , $entry
  }
  # Informational trend for passive logs: MB per hour over periodic samples,
  # skipping the first 10 minutes while caches fill.
  $p.trend = $null
  $px = $series['private_mb'].X; $py = $series['private_mb'].Y
  $tx = New-Object System.Collections.Generic.List[double]; $ty = New-Object System.Collections.Generic.List[double]
  for ($i = 0; $i -lt $px.Count; $i++) { if ($px[$i] -ge 10) { $tx.Add($px[$i] / 60.0); $ty.Add($py[$i]) } }
  if ($tx.Count -ge 10 -and ($tx[$tx.Count - 1] - $tx[0]) -ge 0.5) {
    $fit = Get-LinearFit -X $tx.ToArray() -Y $ty.ToArray()
    if ($fit) { $p.trend = [ordered]@{ mb_per_hour = [Math]::Round($fit.slope, 2); r2 = [Math]::Round($fit.r2, 3); hours = [Math]::Round($tx[$tx.Count - 1] - $tx[0], 2) } }
  }

  # Stress outcome
  $p.finished = $null
  $p.failures = @(); $p.dialogs = @(); $p.page_loads = @(); $p.batches = @(); $p.rebatches = @(); $p.switches = @(); $p.steps = @(); $p.autosaves = @(); $p.thumb_scrolls = @(); $p.cycles_done = 0; $p.generate = $null
  # In reopen mode, cycles after the first re-run Output over pages whose
  # output is already up to date: a cache check, not processing. Those runs
  # ("rebatch": true; older logs: reopen mode, cycle > 1, stage 5) are kept
  # apart so they do not pull the Output throughput down.
  $cfgMode = ''
  if ($cfg -and $cfg.mode) { $cfgMode = [string]$cfg.mode }
  elseif ($runInfo -and $runInfo.params -and $runInfo.params.Mode) { $cfgMode = [string]$runInfo.params.Mode }
  foreach ($s in $p.stress) {
    switch ([string]$s.ev) {
      'stress.finished' { $p.finished = $s }
      'stress.failure' { $p.failures += , $s }
      'stress.unexpected_dialog' { $p.dialogs += , $s }
      'stress.page_load' { $p.page_loads += , $s }
      'stress.batch' {
        $isRe = $false
        if ($null -ne $s.rebatch) { $isRe = ($s.rebatch -eq $true) }
        elseif ($cfgMode -eq 'reopen' -and [int]$s.cycle -gt 1 -and [int]$s.stage -eq 5) { $isRe = $true }
        if ($isRe) { $p.rebatches += , $s } else { $p.batches += , $s }
      }
      'stress.stage_switch' { $p.switches += , $s }
      'stress.step' { $p.steps += , $s }
      'stress.autosave' { $p.autosaves += , $s }
      'stress.thumb_scroll' { $p.thumb_scrolls += , $s }
      'stress.cycle_end' { $p.cycles_done++ }
      'stress.generate' { $p.generate = $s }
    }
  }
  $p.bad_tasks = 0
  foreach ($oc in @('bad_alloc', 'exception', 'unknown')) { if ($p.outcomes.ContainsKey($oc)) { $p.bad_tasks += $p.outcomes[$oc] } }
  $p.page_errors = @($p.page_loads | Where-Object { [string]$_.outcome -ne 'ok' })
  $p.run = $null
  if ($null -ne $p.instance -and $exitByInstance.ContainsKey([int]$p.instance)) { $p.run = $exitByInstance[[int]$p.instance] }

  # Disk I/O: totals, and write rate inside each batch window
  $p.io = [ordered]@{ read_mb = $null; write_mb = $null; batch_write_mbps = @{} }
  $rx = $series['io_write_mb'].X; $ry = $series['io_write_mb'].Y
  if ($ry.Count -ge 2) {
    $p.io.write_mb = [Math]::Round($ry[$ry.Count - 1] - $ry[0], 1)
    $rr = $series['io_read_mb'].Y
    if ($rr.Count -ge 2) { $p.io.read_mb = [Math]::Round($rr[$rr.Count - 1] - $rr[0], 1) }
    foreach ($b in $p.batches) {
      $tEnd = [double]$b.t / 60000.0
      $tBeg = ([double]$b.t - [double]$b.dur) / 60000.0
      $i0 = -1; $i1 = -1
      for ($i = 0; $i -lt $rx.Count; $i++) {
        if ($rx[$i] -le $tBeg) { $i0 = $i }
        if ($i1 -lt 0 -and $rx[$i] -ge $tEnd) { $i1 = $i }
      }
      if ($i0 -lt 0) { $i0 = 0 }
      if ($i1 -lt 0) { $i1 = $rx.Count - 1 }
      $dt = ($rx[$i1] - $rx[$i0]) * 60.0
      if ($dt -gt 0.5) {
        $rate = ($ry[$i1] - $ry[$i0]) / $dt
        $sk = [string]$b.stage
        if (-not $p.io.batch_write_mbps.ContainsKey($sk)) { $p.io.batch_write_mbps[$sk] = @() }
        $p.io.batch_write_mbps[$sk] += $rate
      }
    }
  }
}

# ---------------------------------------------------------------------------
# Checks (the verdict box)
# ---------------------------------------------------------------------------

$StatusRank = @{ 'PASS' = 0; 'N/A' = 0; 'WARN' = 1; 'FAIL' = 2 }
function Get-Worst([string[]]$statuses) {
  $w = 'N/A'
  foreach ($s in $statuses) {
    if ($s -eq 'PASS' -and $w -eq 'N/A') { $w = 'PASS' }
    elseif ($StatusRank[$s] -gt $StatusRank[$w]) { $w = $s }
  }
  return $w
}
$checks = @()
function Add-Check([string]$id, [string]$name, [string]$status, [string]$summary, [string[]]$details = @(), $numbers = $null, [string]$rule = '') {
  $script:checks += , ([ordered]@{ id = $id; name = $name; status = $status; summary = $summary; rule = $rule; details = @($details); numbers = $numbers })
}

# 1. GUI responsiveness
if ($procs.Count -gt 0) {
  $st = @(); $det = @()
  $allCount = 0; $allTotal = 0.0; $allLongest = 0.0; $allOver1 = 0; $allDur = 0.0; $excluded = 0; $exStart = 0; $exShut = 0; $exTest = 0
  $allUnended = 0; $unendedDet = @()
  foreach ($p in $procs) {
    $s = $p.stall_stats
    $status = 'PASS'
    if ($s.longest_ms -gt $Thresholds.StallFailMs -or $s.unended_longest_ms -ge $Thresholds.StallFailMs) { $status = 'FAIL' }
    elseif ($s.longest_ms -gt $Thresholds.StallWarnMs -or $s.unended_longest_ms -ge $Thresholds.StallWarnMs -or $s.pct_of_run -gt $Thresholds.StalledPctWarn) { $status = 'WARN' }
    if ($p.run_ms -le 0 -and $s.unended -eq 0) { $status = 'N/A' }
    $st += $status
    $allCount += $s.count; $allTotal += $s.total_ms; $allOver1 += $s.over_1s; $allDur += $p.run_ms; $excluded += $s.excluded_count
    $exStart += $s.excluded_startup; $exShut += $s.excluded_shutdown; $exTest += $s.excluded_test
    $allUnended += $s.unended
    if ($s.longest_ms -gt $allLongest) { $allLongest = $s.longest_ms }
    $line = $p.id + ': ' + $status + ' - ' + $s.count + ' stalls, total ' + (Format-DiagMs $s.total_ms) + ' (' + (Format-DiagNumber $s.pct_of_run 2) + '% of ' + (Format-DiagMs $p.run_ms) + '), longest ' + (Format-DiagMs $s.longest_ms) + ', ' + $s.over_1s + ' over 1 s'
    foreach ($u in $p.unended) {
      $w = 'still going when the log ended'
      if (-not $u.log_ends) { $w = 'with no end record' }
      $unendedDet += ($p.id + ': ' + (Get-HangText $u) + ', ' + $w)
    }
    $det += $line
  }
  $pctAll = 0.0
  if ($allDur -gt 0) { $pctAll = 100.0 * $allTotal / $allDur }
  $sum = [string]$allCount + ' stalls, total ' + (Format-DiagMs $allTotal) + ' (' + (Format-DiagNumber $pctAll 2) + '% of run time), longest ' + (Format-DiagMs $allLongest) + ', ' + $allOver1 + ' longer than 1 s.'
  if ($allUnended -gt 0) {
    $sum += ' ' + $allUnended + ' stall(s) had not ended when the log did (counted at the length reached, a lower bound): the application was hung.'
  }
  if ($excluded -gt 0) {
    $parts = @()
    if ($exStart -gt 0) { $parts += ([string]$exStart + ' at startup') }
    if ($exShut -gt 0) { $parts += ([string]$exShut + ' at shutdown') }
    if ($exTest -gt 0) { $parts += ([string]$exTest + ' inside the test driver') }
    $sum += ' Not counted, listed in the stalls table: ' + ($parts -join ', ') + '.'
  }
  if ($procs.Count -eq 1) { $det = @() }
  $det = @($unendedDet) + @($det)
  Add-Check 'gui' 'GUI responsiveness' (Get-Worst $st) $sum $det ([ordered]@{ count = $allCount; total_ms = $allTotal; longest_ms = $allLongest; over_1s = $allOver1; pct = [Math]::Round($pctAll, 3); run_ms = [Math]::Round($allDur, 1); unended = $allUnended }) 'WARN: any stall over 1 s, or over 1% of the run time stalled. FAIL: any stall over 5 s. A stall still going when the log ended counts at the length it had reached.'
}

# 2. Leak checks (memory first, then handles/GDI/USER/threads)
foreach ($lm in $Thresholds.LeakMetrics) {
  $st = @(); $det = @(); $nums = @()
  foreach ($p in $procs) {
    $e = $p.leak | Where-Object { $_.metric -eq $lm.key } | Select-Object -First 1
    if ($null -eq $e) { continue }
    if ($p.cycle_end.Count -eq 0) { continue }
    $st += $e.status
    if ($null -ne $e.slope) {
      $line = $p.id + ': ' + $e.status + ' - ' + (Format-DiagNumber $e.slope 2) + ' ' + $lm.unit + '/cycle, r2 ' + (Format-DiagNumber $e.r2 2)
      if ($null -ne $e.slope_all) {
        $line += ' over cycles ' + $e.fit_from + '+ (steady state; ' + (Format-DiagNumber $e.slope_all 2) + '/cycle over all ' + $e.n + ' cycles, including warm-up)'
      } else {
        $line += ' over ' + $e.n + ' cycles'
      }
      $det += ($line + ' (' + (Format-DiagNumber $e.first 0) + ' -> ' + (Format-DiagNumber $e.last 0) + ')')
    } else {
      $det += ($p.id + ': ' + $e.note)
    }
    $nums += , ([ordered]@{ instance = $p.id; slope = $e.slope; r2 = $e.r2; n = $e.n })
  }
  $name = $lm.label + ' growth across cycles'
  $rule = 'Straight-line fit over the end-of-cycle samples, cycle 1 excluded as warm-up; with 10 or more cycles, over the second half only (steady state). WARN: over ' + $lm.warn + ' ' + $lm.unit + '/cycle with r2 over ' + $Thresholds.LeakWarnR2 + '. FAIL: over ' + $lm.fail + ' with r2 over ' + $Thresholds.LeakFailR2 + '.'
  if ($st.Count -eq 0) {
    if ($lm.key -eq 'private_mb') {
      $note = 'No stress cycles in these logs; a leak verdict needs a stress run with at least ' + ($Thresholds.MinLeakCycles + 1) + ' cycles.'
      $trends = @($procs | Where-Object { $_.trend } | ForEach-Object { $_.id + ': ' + (Format-DiagNumber $_.trend.mb_per_hour 1) + ' MB/hour over ' + (Format-DiagNumber $_.trend.hours 1) + ' h (r2 ' + (Format-DiagNumber $_.trend.r2 2) + ')' })
      if ($trends.Count -gt 0) { $note += ' Informational private-memory trend per session is listed below.' }
      Add-Check ('leak_' + $lm.key) $name 'N/A' $note $trends $null $rule
    }
    continue
  }
  $worst = Get-Worst $st
  $valid = @($nums | Where-Object { $null -ne $_.slope })
  if ($valid.Count -eq 0) { $summaryText = 'Insufficient cycles for a verdict (need ' + $Thresholds.MinLeakCycles + ' after the warm-up cycle).'; $worst = 'N/A' }
  else {
    $top = $valid | Sort-Object { [double]$_.slope } -Descending | Select-Object -First 1
    $summaryText = 'Steepest: ' + (Format-DiagNumber $top.slope 2) + ' ' + $lm.unit + '/cycle (r2 ' + (Format-DiagNumber $top.r2 2) + ', ' + $top.instance + ').'
    # Measured on this application: Qt's font, glyph and pixmap caches and the
    # heap keep growing for the first 10-20 cycles and then level off. A short
    # run cannot tell that from a leak, and the report must not pretend it can.
    $fewest = ($valid | ForEach-Object { [int]$_.n } | Measure-Object -Minimum).Minimum
    if ($worst -ne 'PASS' -and $fewest -lt 15) {
      $summaryText += ' Only ' + $fewest + ' cycles after warm-up: caches typically keep growing for the first 10-20 cycles and then level off, so this is not conclusive. Re-run with -Cycles 25 for a leak verdict.'
    }
  }
  Add-Check ('leak_' + $lm.key) $name $worst $summaryText $det $nums $rule
}

# 3. Failures: crashes / exit codes, task failures, dialogs, page errors
$crashDet = @()
$crashStatus = 'PASS'
$cnt = @{ crashed = 0; failures = 0; fatal = 0; timeout = 0; nostop = 0; unfinished = 0; oom = 0; killed = 0; nolog = 0; hung = 0; noexit = 0 }
function Get-ExitCodeText($code) {
  # Meaning of a stress instance's exit code, and which counter it goes to.
  $c = [long]$code
  if ($c -eq 2) { return @('failures', 'exit code 2 (scenario finished with failures)') }
  if ($c -eq 3) { return @('fatal', 'exit code 3 (fatal: configuration, out of memory or project creation)') }
  return @('crashed', ('exit code ' + (Format-ExitCode $code) + ' - the process crashed'))
}
foreach ($p in $procs) {
  # The longest stall that was still going when this log ended, if any: the
  # explanation for a missing stop record or a timeout.
  $hang = $null
  foreach ($u in $p.unended) { if ($u.log_ends -and ($null -eq $hang -or [double]$u.dur -gt [double]$hang.dur)) { $hang = $u } }
  # Killed by the runner because the run was aborted (Ctrl+C, runner error),
  # as opposed to the timeout: not something the application did.
  $killed = $false
  if ($p.run) {
    $code = $p.run.exit_code
    $killed = ([bool]$p.run.killed -and -not [bool]$p.run.timed_out)
    if ($p.run.timed_out) {
      $crashStatus = 'FAIL'; $cnt.timeout++
      $crashDet += ($p.id + ': killed after the overall timeout (' + [string]$p.run.timeout_minutes + ' min)' + $(if ($hang) { ', during ' + (Get-HangText $hang) } else { '' }))
    } elseif ($killed) {
      if ($crashStatus -eq 'PASS') { $crashStatus = 'WARN' }
      $cnt.killed++
      $crashDet += ($p.id + ': killed by the runner (run aborted) - not an application crash' + $(if ($hang) { '; it was in ' + (Get-HangText $hang) + ' at the time' } else { '' }))
    } elseif ($null -eq $code) { $crashStatus = 'FAIL'; $cnt.noexit++; $crashDet += ($p.id + ': no exit code recorded') }
    elseif ([long]$code -eq 0) { }
    else {
      $ec = Get-ExitCodeText $code
      $crashStatus = 'FAIL'; $cnt[$ec[0]]++; $crashDet += ($p.id + ': ' + $ec[1])
    }
  }
  # A process the runner killed has no stop or stress.finished record by
  # construction; that is reported above, not as a failure of its own.
  if ($isStress -and $p.config -and $null -eq $p.finished -and -not $killed) { $crashStatus = 'FAIL'; $cnt.unfinished++; $crashDet += ($p.id + ': no stress.finished record - the scenario did not complete') }
  if ($p.finished -and -not $p.finished.ok) { $crashStatus = 'FAIL'; $crashDet += ($p.id + ': stress.finished ok=false, ' + [string]$p.finished.failures + ' failure(s)') }
  if ($null -eq $p.stop -and -not $killed) {
    $cnt.nostop++
    if ($isStress) { $crashStatus = 'FAIL' } elseif ($crashStatus -eq 'PASS') { $crashStatus = 'WARN' }
    if ($hang) {
      $cnt.hung++
      $crashDet += ($p.id + ': log ends during ' + (Get-HangText $hang) + ' - the application was hung, which is why there is no stop record')
    } else {
      $crashDet += ($p.id + ': log ends without a stop record (crash, forced close, power loss, or still running)')
    }
  }
  if ($p.truncated) { $crashDet += ($p.id + ': last log line truncated') }
  if ($p.oom.Count -gt 0) { $crashStatus = 'FAIL'; $cnt.oom++; $crashDet += ($p.id + ': ' + $p.oom.Count + ' out-of-memory event(s)') }
}
# Instances the runner started (or tried to) that left no diagnostics log:
# a start failure, or a process that died or was killed before logging.
# Without this they would not appear anywhere and the run could pass.
$procInst = @{}
foreach ($p in $procs) { if ($null -ne $p.instance) { $procInst[[int]$p.instance] = $true } }
$noLogInstances = @()
foreach ($k in @($exitByInstance.Keys | Sort-Object)) {
  if ($procInst.ContainsKey([int]$k)) { continue }
  $ri = $exitByInstance[$k]
  $id = 'inst' + $k
  $cnt.nolog++
  $why = ''
  if ($ri.start_error) {
    $crashStatus = 'FAIL'
    $why = 'could not be started: ' + [string]$ri.start_error
  } elseif ($ri.timed_out) {
    $crashStatus = 'FAIL'; $cnt.timeout++
    $why = 'killed after the overall timeout (' + [string]$ri.timeout_minutes + ' min) and wrote no diagnostics log'
  } elseif ($ri.killed) {
    if ($crashStatus -eq 'PASS') { $crashStatus = 'WARN' }
    $cnt.killed++
    $why = 'killed by the runner (run aborted) before it wrote a diagnostics log - not an application crash'
  } elseif ($null -eq $ri.exit_code) {
    $crashStatus = 'FAIL'; $cnt.noexit++
    $why = 'no exit code recorded and wrote no diagnostics log'
  } elseif ([long]$ri.exit_code -eq 0) {
    $crashStatus = 'FAIL'
    $why = 'exit code 0 but wrote no diagnostics log (diagnostics off, or its log folder not writable)'
  } else {
    $ec = Get-ExitCodeText $ri.exit_code
    $crashStatus = 'FAIL'; $cnt[$ec[0]]++
    $why = $ec[1] + ', and wrote no diagnostics log'
  }
  $crashDet += ($id + ': ' + $why)
  $noLogInstances += , ([ordered]@{ instance = [int]$k; id = $id; reason = $why; start_error = $ri.start_error; exit_code = $ri.exit_code; timed_out = [bool]$ri.timed_out; killed = [bool]$ri.killed })
}
if ($crashFiles.Count -gt 0) {
  $crashStatus = 'FAIL'
  $crashDet += ([string]$crashFiles.Count + ' crash report file(s): ' + (($crashFiles | Select-Object -First 8 | ForEach-Object { $_.Name }) -join ', '))
}
$parts = @()
if ($cnt.crashed -gt 0) { $parts += ([string]$cnt.crashed + ' crashed') }
if ($cnt.fatal -gt 0) { $parts += ([string]$cnt.fatal + ' ended with a fatal error') }
if ($cnt.failures -gt 0) { $parts += ([string]$cnt.failures + ' finished with failures (exit code 2)') }
if ($cnt.timeout -gt 0) { $parts += ([string]$cnt.timeout + ' killed at the timeout') }
if ($cnt.noexit -gt 0) { $parts += ([string]$cnt.noexit + ' without a recorded exit code') }
if ($cnt.unfinished -gt 0) { $parts += ([string]$cnt.unfinished + ' did not finish the scenario') }
if ($cnt.oom -gt 0) { $parts += ([string]$cnt.oom + ' ran out of memory') }
if ($cnt.hung -gt 0) { $parts += ([string]$cnt.hung + ' log(s) end during a GUI stall (hung)') }
if ($cnt.nostop -gt $cnt.hung) { $parts += ([string]($cnt.nostop - $cnt.hung) + ' log(s) end without a stop record') }
if ($cnt.nolog -gt 0) { $parts += ([string]$cnt.nolog + ' instance(s) wrote no diagnostics log') }
if ($cnt.killed -gt 0) { $parts += ([string]$cnt.killed + ' killed by the runner when the run was aborted (not a crash)') }
if ($crashFiles.Count -gt 0) { $parts += ([string]$crashFiles.Count + ' crash report file(s)') }
$crashSummary = 'No crash reports; every process ended normally.'
if ($parts.Count -gt 0) {
  $of = [string]$procs.Count + ' process(es)'
  if ($cnt.nolog -gt 0) { $of += ' with a log and ' + $cnt.nolog + ' instance(s) without one' }
  $crashSummary = 'Of ' + $of + ': ' + ($parts -join '; ') + '.'
}
# Nothing to judge only when there is nothing at all: a crash report or an
# instance that never logged is a finding even without any process log.
if ($procs.Count -eq 0 -and $crashStatus -eq 'PASS') { $crashStatus = 'N/A'; $crashSummary = 'No process logs and no crash reports.' }
Add-Check 'crashes' 'Crashes and exit codes' $crashStatus $crashSummary $crashDet ([ordered]@{ crash_files = $crashFiles.Count; crashed = $cnt.crashed; fatal = $cnt.fatal; failures = $cnt.failures; timed_out = $cnt.timeout; no_exit_code = $cnt.noexit; unfinished = $cnt.unfinished; oom = $cnt.oom; no_stop = $cnt.nostop; hung = $cnt.hung; no_log = $cnt.nolog; killed = $cnt.killed }) 'FAIL: any crash, crash report, fatal exit, timeout, unfinished scenario, or stress instance that could not start or wrote no log. WARN: an instance killed by the runner because the run was aborted. In everyday logs a session without a stop record is WARN (it may also be a forced close or power loss).'

$taskDet = @(); $taskBad = 0; $taskNull = 0; $taskTotal = 0
foreach ($p in $procs) {
  foreach ($k in $p.outcomes.Keys) { $taskTotal += $p.outcomes[$k] }
  if ($p.outcomes.ContainsKey('null')) { $taskNull += $p.outcomes['null'] }
  if ($p.bad_tasks -gt 0) {
    $taskBad += $p.bad_tasks
    $parts = @($p.outcomes.Keys | Where-Object { $_ -ne 'ok' -and $_ -ne 'cancelled' } | ForEach-Object { $_ + ' ' + $p.outcomes[$_] })
    $taskDet += ($p.id + ': ' + ($parts -join ', '))
  }
  foreach ($f in $p.failures) { $taskDet += ($p.id + ': stress.failure ' + [string]$f.what + ' (cycle ' + [string]$f.cycle + ', step ' + [string]$f.step + ')') }
}
$failCount = 0
foreach ($p in $procs) { $failCount += $p.failures.Count }
$tStatus = 'PASS'
if ($taskBad -gt 0 -or $failCount -gt 0) { $tStatus = 'FAIL' }
# At the basic level a task.run record is written for every task that did
# not end ok or cancelled, but for the others only when slow: the failures
# are complete, the number of records is not the number of tasks. The
# aggregates count every task.
$basicProcs = @($procs | Where-Object { $_.level -eq 'basic' }).Count
$taskAgg = $null
if ($aggStats.ContainsKey('task.run')) { $taskAgg = [long]$aggStats['task.run'].n }
if ($taskTotal -eq 0 -and $failCount -eq 0 -and -not ($taskAgg -gt 0)) { $tStatus = 'N/A' }
if ($basicProcs -gt 0) {
  $tSum = [string]$taskTotal + ' task.run record(s): at the basic level (' + $basicProcs + ' of ' + $procs.Count + ' process(es)) only slow and failed tasks are written, so this is not the number of tasks'
  if ($null -ne $taskAgg) { $tSum += ' (' + $taskAgg + ' ran in total, from the aggregates)' }
  $tSum += '; failures are always written. ' + $taskBad + ' ended in bad_alloc/exception/unknown; ' + $failCount + ' stress failure record(s).'
} else {
  $tSum = [string]$taskTotal + ' background tasks recorded; ' + $taskBad + ' ended in bad_alloc/exception/unknown; ' + $failCount + ' stress failure record(s).'
}
if ($taskNull -gt 0) { $tSum += ' ' + $taskNull + ' returned no result (outcome null; informational).' }
Add-Check 'tasks' 'Task failures' $tStatus $tSum $taskDet ([ordered]@{ tasks = $taskTotal; tasks_all = $taskAgg; basic_level_processes = $basicProcs; bad = $taskBad; stress_failures = $failCount; null_results = $taskNull }) 'FAIL: any background task ending in bad_alloc, exception or unknown, or any stress failure record.'

$dlgDet = @()
$dlgCount = 0
foreach ($p in $procs) {
  foreach ($d in $p.dialogs) {
    $dlgCount++
    $dlgDet += ($p.id + ': "' + [string]$d.title + '" - ' + [string]$d.text + ' (' + [string]$d.class + ', action ' + [string]$d.action + ')')
  }
}
if ($isStress) {
  $dStatus = 'PASS'
  if ($dlgCount -gt 0) { $dStatus = 'FAIL' }
  $dSum = [string]$dlgCount + ' unexpected dialog(s) during the scenario.'
  # No process log, no scenario to judge: PASS here would read as "checked".
  if ($procs.Count -eq 0) { $dStatus = 'N/A'; $dSum = 'No process logs, so no scenario to check.' }
  Add-Check 'dialogs' 'Unexpected dialogs' $dStatus $dSum $dlgDet ([ordered]@{ count = $dlgCount }) 'FAIL: any dialog the scenario did not expect.'

  $peDet = @(); $peCount = 0; $plTotal = 0
  foreach ($p in $procs) {
    $plTotal += $p.page_loads.Count
    foreach ($e in $p.page_errors) {
      $peCount++
      $sn = [string]$e.stage_name
      if ([int]$e.stage -ge 0 -and [int]$e.stage -lt $StageNames.Count) { $sn = $StageNames[[int]$e.stage] }
      if ($peDet.Count -lt 30) { $peDet += ($p.id + ': cycle ' + [string]$e.cycle + ', ' + $sn + ', page ' + [string]$e.index + ': ' + [string]$e.outcome + ' after ' + (Format-DiagMs $e.dur)) }
    }
  }
  $peStatus = 'PASS'
  if ($peCount -gt 0) { $peStatus = 'FAIL' }
  if ($plTotal -eq 0) { $peStatus = 'N/A' }
  Add-Check 'page_loads' 'Page loads (timeouts and errors)' $peStatus ([string]$peCount + ' of ' + $plTotal + ' page loads did not end ok.') $peDet ([ordered]@{ total = $plTotal; failed = $peCount }) 'FAIL: any page load that timed out or ended in an error.'
}

$dropDet = @(); $dropTotal = 0.0
foreach ($p in $procs) {
  if ($p.log_dropped -gt 0) { $dropTotal += $p.log_dropped; $dropDet += ($p.id + ': ' + $p.log_dropped + ' records dropped (max backlog ' + $p.log_queue_max + ')') }
}
$badLines = 0
foreach ($p in $procs) { $badLines += $p.bad }
$ldStatus = 'PASS'
if ($dropTotal -gt 0) { $ldStatus = 'WARN' }
if ($procs.Count -eq 0) { $ldStatus = 'N/A' }
$ldSum = [string]$dropTotal + ' diagnostics records dropped because the backlog was full; ' + $badLines + ' unreadable line(s).'
if ($dropTotal -gt 0) { $ldSum += ' Counts in this report are then lower bounds.' }
Add-Check 'log_drops' 'Diagnostics log completeness' $ldStatus $ldSum $dropDet ([ordered]@{ dropped = $dropTotal; bad_lines = $badLines }) 'WARN: any record dropped because the diagnostics backlog was full.'

$overall = Get-Worst @($checks | ForEach-Object { $_.status })
# Checks that fail a CI job: things that went wrong, as opposed to timings and
# trends, which a busy or shared machine can push over a threshold by itself.
$HardChecks = @('crashes', 'tasks', 'dialogs', 'page_loads')
$hardFail = @($checks | Where-Object { $HardChecks -contains $_.id -and $_.status -eq 'FAIL' }).Count -gt 0

# A run the runner did not see through: the numbers cover only part of it.
$aborted = $false
$partialNote = ''
if ($runInfo) {
  if ($runInfo.aborted) {
    $aborted = $true
    $partialNote = 'Run aborted before completion - results are partial. The runner stopped the instances that were still running (Ctrl+C or a runner error); see the crash check for which.'
  } elseif (-not $runInfo.ended) {
    $partialNote = 'The test runner did not finish (run.json has no end time): it was interrupted or is still running, so results are partial.'
  }
}

# ---------------------------------------------------------------------------
# Cross-process tables
# ---------------------------------------------------------------------------

# Stalls with their process, longest first
$allStalls = @()
foreach ($p in $procs) {
  foreach ($s in $p.stalls) {
    $allStalls += , @{ proc = $p; s = $s; dur = [double]$s.dur; cls = (Get-StallClass $s) }
  }
}
$allStalls = @($allStalls | Sort-Object { $_.dur } -Descending)

function Get-StallWhen($p, $s) {
  $at = [double]$s.at
  if ($null -eq $s.at) { $at = [double]$s.t - [double]$s.dur }
  $rel = 't+' + (Format-DiagMs ($at - $p.first_t))
  if ($p.wall_start) { return $p.wall_start.AddMilliseconds($at).ToString('yyyy-MM-dd HH:mm:ss', $Inv) }
  return $rel
}

$histEdges = @(0, 500, 1000, 2000, 5000, 15000)
$histLabels = @('< 0.5 s', '0.5-1 s', '1-2 s', '2-5 s', '5-15 s', '>= 15 s')
$hist = New-Object double[] 6
$byOpAny = @{}; $byOpInner = @{}
foreach ($x in $allStalls) {
  $d = $x.dur
  $b = 0
  for ($i = $histEdges.Count - 1; $i -ge 0; $i--) { if ($d -ge $histEdges[$i]) { $b = $i; break } }
  $hist[$b]++
}
# The ranking explains the verdict, so it uses the stalls the verdict counts;
# when none are counted it falls back to all of them.
$rankStalls = @($allStalls | Where-Object { $_.cls -eq 'run' })
$rankAll = $false
if ($rankStalls.Count -eq 0) { $rankStalls = $allStalls; $rankAll = $true }
foreach ($x in $rankStalls) {
  $d = $x.dur
  $ops = @($x.s.ops | Where-Object { $_ })
  $opsLast = @($x.s.ops_last | Where-Object { $_ })
  $set = @{}
  foreach ($o in ($ops + $opsLast)) { $set[[string]$o] = $true }
  if ($set.Count -eq 0) { $set['(no instrumented operation open)'] = $true }
  foreach ($o in $set.Keys) {
    if (-not $byOpAny.ContainsKey($o)) { $byOpAny[$o] = @{ n = 0; ms = 0.0; max = 0.0 } }
    $byOpAny[$o].n++; $byOpAny[$o].ms += $d; if ($d -gt $byOpAny[$o].max) { $byOpAny[$o].max = $d }
  }
  $inner = '(no instrumented operation open)'
  if ($opsLast.Count -gt 0) { $inner = [string]$opsLast[$opsLast.Count - 1] } elseif ($ops.Count -gt 0) { $inner = [string]$ops[$ops.Count - 1] }
  if (-not $byOpInner.ContainsKey($inner)) { $byOpInner[$inner] = @{ n = 0; ms = 0.0; max = 0.0 } }
  $byOpInner[$inner].n++; $byOpInner[$inner].ms += $d; if ($d -gt $byOpInner[$inner].max) { $byOpInner[$inner].max = $d }
}

# Operation timing rows
$opRows = @()
function Get-KeyStats($lists, $over, $key) {
  # Exact n/sum/max (from the overflow totals once sampling began) and
  # percentiles from the list, which is then a uniform sample.
  $s = Get-SampleStats -Values $lists[$key]
  $ov = $over[$key]
  $r = @{ n = $s.n; sum = $s.sum; max = $s.max; p50 = $s.p50; p95 = $s.p95; p99 = $s.p99; sampled = $false }
  if ($null -ne $ov) { $r.n = [long]$ov[0]; $r.sum = $ov[1]; $r.max = $ov[2]; $r.sampled = $true }
  return $r
}
foreach ($key in $opDur.Keys) {
  $st = Get-KeyStats $opDur $opOver $key
  $nm = $opName[$key]
  $opRows += , ([ordered]@{
      name = $nm[0]; th = $nm[1]; n = $st.n; sum = [Math]::Round($st.sum, 1); mean = [Math]::Round($st.sum / [Math]::Max(1, $st.n), 2)
      p50 = $st.p50; p95 = $st.p95; p99 = $st.p99; max = [Math]::Round([double]$st.max, 1); sampled = $st.sampled
    })
}
$opRows = @($opRows | Sort-Object @{ Expression = { if ($_.th -eq 'gui') { 0 } else { 1 } } }, @{ Expression = { $_.th } }, @{ Expression = { $_.sum }; Descending = $true })
$aggRows = @()
foreach ($k in $aggStats.Keys) {
  $a = $aggStats[$k]
  $aggRows += , ([ordered]@{ name = $k; n = $a.n; sum = [Math]::Round($a.sum, 1); mean = if ($a.n -gt 0) { [Math]::Round($a.sum / $a.n, 3) } else { $null }; max = [Math]::Round($a.max, 1) })
}
$aggRows = @($aggRows | Sort-Object { $_.sum } -Descending)

# Stage self time per page. The aggregates count every call, so comparing
# them with the records tells whether every stage call has a record: true at
# every level in current builds (stage records are always written), not in
# basic-level logs from older builds, or when records were dropped.
$stageRows = @()
for ($i = 0; $i -lt $StageOps.Count; $i++) {
  if (-not $stageDur.ContainsKey($StageOps[$i])) { continue }
  $st = Get-KeyStats $stageDur $stageOver $StageOps[$i]
  $callsAll = $null
  if ($aggStats.ContainsKey($StageOps[$i])) { $callsAll = [long]$aggStats[$StageOps[$i]].n }
  $complete = ($null -eq $callsAll -or $callsAll -le $st.n)
  $stageRows += , ([ordered]@{ stage = $i; name = $StageNames[$i]; op = $StageOps[$i]; n = $st.n; calls_all = $callsAll; complete = $complete; mean = [Math]::Round($st.sum / [Math]::Max(1, $st.n), 1); p50 = $st.p50; p95 = $st.p95; max = [Math]::Round([double]$st.max, 1) })
}

# Batch throughput
$batchRows = @()
$batchByStage = @{}
foreach ($p in $procs) {
  foreach ($b in $p.batches) {
    $sk = [int]$b.stage
    if (-not $batchByStage.ContainsKey($sk)) { $batchByStage[$sk] = @{ name = [string]$b.stage_name; v = @(); pages = 0; runs = 0; failed = 0 } }
    $e = $batchByStage[$sk]
    if ($null -ne $b.sec_per_page) { $e.v += [double]$b.sec_per_page }
    $e.pages += [int]$b.pages; $e.runs++
    if ($b.ok -eq $false) { $e.failed++ }
  }
}
foreach ($sk in ($batchByStage.Keys | Sort-Object)) {
  $e = $batchByStage[$sk]
  $s = Get-SampleStats -Values $e.v
  $nm = $e.name
  if ($sk -ge 0 -and $sk -lt $StageNames.Count) { $nm = $StageNames[$sk] }
  $batchRows += , ([ordered]@{ stage = $sk; name = $nm; runs = $e.runs; pages = $e.pages; failed = $e.failed; mean = $s.mean; min = $s.min; max = $s.max; p50 = $s.p50 })
}
# Re-checks of already finished output (reopen mode), kept out of the rows
# above: they measure the output cache check, not processing.
$recheckRows = @()
$recheckBy = @{}
foreach ($p in $procs) {
  foreach ($b in $p.rebatches) {
    $sk = [int]$b.stage
    if (-not $recheckBy.ContainsKey($sk)) { $recheckBy[$sk] = @{ v = @(); pages = 0; runs = 0; failed = 0 } }
    $e = $recheckBy[$sk]
    if ($null -ne $b.sec_per_page) { $e.v += [double]$b.sec_per_page }
    $e.pages += [int]$b.pages; $e.runs++
    if ($b.ok -eq $false) { $e.failed++ }
  }
}
foreach ($sk in ($recheckBy.Keys | Sort-Object)) {
  $e = $recheckBy[$sk]
  $s = Get-SampleStats -Values $e.v
  $nm = [string]$sk
  if ($sk -ge 0 -and $sk -lt $StageNames.Count) { $nm = $StageNames[$sk] }
  $recheckRows += , ([ordered]@{ stage = $sk; name = ($nm + ' (re-check of finished output)'); runs = $e.runs; pages = $e.pages; failed = $e.failed; mean = $s.mean; min = $s.min; max = $s.max; p50 = $s.p50 })
}

# Page load and stage switch latency
$plRows = @()
$plByStage = @{}
foreach ($p in $procs) {
  foreach ($e in $p.page_loads) {
    $sk = [int]$e.stage
    if (-not $plByStage.ContainsKey($sk)) { $plByStage[$sk] = @{ name = [string]$e.stage_name; v = @(); outcomes = @{} } }
    $plByStage[$sk].v += [double]$e.dur
    $oc = [string]$e.outcome
    if ($plByStage[$sk].outcomes.ContainsKey($oc)) { $plByStage[$sk].outcomes[$oc]++ } else { $plByStage[$sk].outcomes[$oc] = 1 }
  }
}
foreach ($sk in ($plByStage.Keys | Sort-Object)) {
  $e = $plByStage[$sk]
  $s = Get-SampleStats -Values $e.v
  $oc = @($e.outcomes.Keys | Sort-Object | ForEach-Object { $_ + ' ' + $e.outcomes[$_] }) -join ', '
  $nm = $e.name
  if ($sk -ge 0 -and $sk -lt $StageNames.Count) { $nm = $StageNames[$sk] }
  $plRows += , ([ordered]@{ stage = $sk; name = $nm; n = $s.n; p50 = $s.p50; p95 = $s.p95; max = $s.max; outcomes = $oc })
}
$swRows = @()
$swBy = @{}
foreach ($p in $procs) {
  foreach ($e in $p.switches) {
    $k = [string]$e.from + '>' + [string]$e.to
    if (-not $swBy.ContainsKey($k)) { $swBy[$k] = @{ from = [int]$e.from; to = [int]$e.to; v = @() } }
    $swBy[$k].v += [double]$e.dur
  }
}
foreach ($k in $swBy.Keys) {
  $e = $swBy[$k]
  $s = Get-SampleStats -Values $e.v
  $swRows += , ([ordered]@{ from = $e.from; to = $e.to; n = $s.n; p50 = $s.p50; p95 = $s.p95; max = $s.max })
}
$swRows = @($swRows | Sort-Object { $_.from }, { $_.to })
$stepRows = @()
$stepBy = @{}
foreach ($p in $procs) {
  foreach ($e in $p.steps) {
    $k = [string]$e.step
    if (-not $stepBy.ContainsKey($k)) { $stepBy[$k] = @{ v = @(); failed = 0 } }
    $stepBy[$k].v += [double]$e.dur
    if ($e.ok -eq $false) { $stepBy[$k].failed++ }
  }
  # Autosaves by branch (what the autosave decided to do). "ok" is whether it
  # wrote something, so a branch that skips by design is never "not ok".
  foreach ($e in $p.autosaves) {
    $br = [string]$e.branch
    $k = 'autosave'
    if ($br) { $k = 'autosave: ' + $br }
    if (-not $stepBy.ContainsKey($k)) { $stepBy[$k] = @{ v = @(); failed = 0; skip = $false } }
    $stepBy[$k].v += [double]$e.dur
    if ($AutosaveSkipBranches -contains $br) { $stepBy[$k].skip = $true }
    elseif ($e.ok -eq $false) { $stepBy[$k].failed++ }
  }
  if ($p.thumb_scrolls.Count -gt 0) {
    if (-not $stepBy.ContainsKey('thumb_scroll')) { $stepBy['thumb_scroll'] = @{ v = @(); failed = 0 } }
    foreach ($e in $p.thumb_scrolls) { $stepBy['thumb_scroll'].v += [double]$e.dur }
  }
}
foreach ($k in ($stepBy.Keys | Sort-Object)) {
  $s = Get-SampleStats -Values $stepBy[$k].v
  $stepRows += , ([ordered]@{ step = $k; n = $s.n; failed = $stepBy[$k].failed; skips_by_design = [bool]$stepBy[$k].skip; p50 = $s.p50; p95 = $s.p95; max = $s.max })
}

# Per-cycle samples
$cycleRows = @()
foreach ($p in $procs) {
  $ends = @{}
  foreach ($s in $p.stress) { if ($s.ev -eq 'stress.cycle_end') { $ends[[int]$s.cycle] = [double]$s.dur } }
  foreach ($c in $p.cycle_end) {
    $cycleRows += , ([ordered]@{
        instance = $p.id; cycle = [int]$c.cycle; dur_ms = $ends[[int]$c.cycle]
        private_mb = Get-Num $c.private_mb; ws_mb = Get-Num $c.ws_mb; handles = Get-Num $c.handles; gdi = Get-Num $c.gdi
        user = Get-Num $c.user; threads = Get-Num $c.threads; sys_avail_mb = Get-Num $c.sys_avail_mb
      })
  }
}

# ---------------------------------------------------------------------------
# summary.json
# ---------------------------------------------------------------------------

$firstStart = $null
foreach ($p in $procs) { if ($p.start) { $firstStart = $p.start; break } }
$firstSettings = $null
foreach ($p in $procs) { if ($p.settings) { $firstSettings = $p.settings; break } }
$firstGenerate = $null
foreach ($p in $procs) { if ($p.generate) { $firstGenerate = $p.generate; break } }
# libtiff compression codes, for stress.generate "compression".
$TiffCompression = @{ 1 = 'none'; 2 = 'CCITT RLE'; 3 = 'CCITT G3'; 4 = 'CCITT G4'; 5 = 'LZW'; 6 = 'old JPEG'; 7 = 'JPEG'; 8 = 'Deflate'; 32773 = 'PackBits'; 32946 = 'Deflate (old code)' }
function Format-TiffCompression($code) {
  if ($null -eq $code -or "$code" -eq '') { return $null }
  $n = $null
  try { $n = [int]$code } catch { return [string]$code }
  $nm = $TiffCompression[$n]
  if ($nm) { return ($nm + ' (' + $n + ')') }
  return ('code ' + $n)
}

function ConvertTo-PlainMap($o) {
  # Record (Dictionary or PSCustomObject) -> ordered map of scalar fields for JSON.
  if ($null -eq $o) { return $null }
  $m = [ordered]@{}
  if ($o -is [System.Collections.IDictionary]) {
    foreach ($k in $o.Keys) { $m[[string]$k] = $o[$k] }
  } else {
    foreach ($pp in $o.PSObject.Properties) { $m[$pp.Name] = $pp.Value }
  }
  foreach ($k in @($m.Keys)) {
    if ($m[$k] -is [datetime]) { $m[$k] = $m[$k].ToString('yyyy-MM-ddTHH:mm:ss.fffzzz', $Inv) }
  }
  return $m
}

$instSummaries = @()
foreach ($p in $procs) {
  $plAll = @()
  foreach ($e in $p.page_loads) { $plAll += [double]$e.dur }
  $pls = Get-SampleStats -Values $plAll
  $bsp = [ordered]@{}
  foreach ($b in ($p.batches | Sort-Object { [int]$_.stage })) {
    $k = [string]$b.stage
    if (-not $bsp.Contains($k)) { $bsp[$k] = @() }
    $bsp[$k] += [double]$b.sec_per_page
  }
  $bspMean = [ordered]@{}
  foreach ($k in $bsp.Keys) { $bspMean[$k] = [Math]::Round((Get-SampleStats -Values $bsp[$k]).mean, 3) }
  $rcv = @()
  foreach ($b in $p.rebatches) { if ($null -ne $b.sec_per_page) { $rcv += [double]$b.sec_per_page } }
  $rcMean = $null
  if ($rcv.Count -gt 0) { $rcMean = [Math]::Round((Get-SampleStats -Values $rcv).mean, 3) }
  $instSummaries += , ([ordered]@{
      id = $p.id; instance = $p.instance; pid = $p.pid; level = $p.level
      wall_start = if ($p.wall_start) { $p.wall_start.ToString('yyyy-MM-ddTHH:mm:sszzz', $Inv) } else { $null }
      duration_s = [Math]::Round($p.duration_ms / 1000.0, 1); records = $p.records; bad_lines = $p.bad; truncated = $p.truncated
      stopped_cleanly = ($null -ne $p.stop)
      exit_code = if ($p.run) { $p.run.exit_code } else { $null }
      timed_out = if ($p.run) { [bool]$p.run.timed_out } else { $null }
      killed = if ($p.run) { [bool]$p.run.killed } else { $null }
      log_ends_in_stall = (@($p.unended | Where-Object { $_.log_ends }).Count -gt 0)
      cycles_done = $p.cycles_done
      finished_ok = if ($p.finished) { [bool]$p.finished.ok } else { $null }
      stalls = $p.stall_stats
      beat = $p.beat
      leak = $p.leak
      trend = $p.trend
      peak_ws_mb = $p.peak_ws_mb
      log_dropped = $p.log_dropped
      failures = $p.failures.Count; dialogs = $p.dialogs.Count; page_errors = $p.page_errors.Count; bad_tasks = $p.bad_tasks
      task_outcomes = $p.outcomes
      page_load = [ordered]@{ n = $pls.n; p50 = $pls.p50; p95 = $pls.p95; max = $pls.max }
      batch_sec_per_page = $bspMean
      batch_recheck_sec_per_page = $rcMean
      io = [ordered]@{ read_mb = $p.io.read_mb; write_mb = $p.io.write_mb }
    })
}

$stallSummary = [ordered]@{
  count = $allStalls.Count
  histogram = [ordered]@{}
  by_innermost_op = @()
  by_any_op = @()
  top = @()
}
for ($i = 0; $i -lt $histLabels.Count; $i++) { $stallSummary.histogram[$histLabels[$i]] = $hist[$i] }
foreach ($k in ($byOpInner.Keys | Sort-Object { $byOpInner[$_].ms } -Descending)) { $stallSummary.by_innermost_op += , ([ordered]@{ op = $k; n = $byOpInner[$k].n; ms = [Math]::Round($byOpInner[$k].ms, 1); max = $byOpInner[$k].max }) }
foreach ($k in ($byOpAny.Keys | Sort-Object { $byOpAny[$_].ms } -Descending)) { $stallSummary.by_any_op += , ([ordered]@{ op = $k; n = $byOpAny[$k].n; ms = [Math]::Round($byOpAny[$k].ms, 1); max = $byOpAny[$k].max }) }
foreach ($x in ($allStalls | Select-Object -First 50)) {
  $stallSummary.top += , ([ordered]@{ instance = $x.proc.id; when = Get-StallWhen $x.proc $x.s; dur = $x.dur; unended = [bool]$x.s.unended; phase = [string]$x.s.phase; counted = ($x.cls -eq 'run'); attributed_to = $StallClassText[$x.cls]; ops = @($x.s.ops); ops_last = @($x.s.ops_last); stack = @($x.s.stack | Select-Object -First 12) })
}

$summary = [ordered]@{
  schema = 1
  tool = 'Analyze-Diagnostics ' + (Get-DiagSuiteVersion)
  generated = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz', $Inv)
  source = $Path
  label = $label
  computer = if ($inventory -and $inventory.computer) { [string]$inventory.computer } elseif ($manifest -and $manifest.computer) { [string]$manifest.computer } else { '' }
  kind = if ($isStress) { 'stress' } else { 'passive' }
  aborted = $aborted
  partial_note = $partialNote
  verdict = [ordered]@{ overall = $overall; hard_fail = $hardFail; hard_checks = $HardChecks; checks = $checks }
  thresholds = [ordered]@{ stall_warn_ms = $Thresholds.StallWarnMs; stall_fail_ms = $Thresholds.StallFailMs; stalled_pct_warn = $Thresholds.StalledPctWarn; min_leak_cycles = $Thresholds.MinLeakCycles }
  environment = [ordered]@{
    start = ConvertTo-PlainMap $firstStart
    settings = ConvertTo-PlainMap $firstSettings
    screens = @(if ($procs.Count -gt 0) { $procs[0].screens | ForEach-Object { ConvertTo-PlainMap $_ } })
    generate = ConvertTo-PlainMap $firstGenerate
    inventory = $inventory
  }
  run = if ($runInfo) { [ordered]@{ params = $runInfo.params; started = $runInfo.started; ended = $runInfo.ended; aborted = [bool]$runInfo.aborted; exe = $runInfo.exe; exe_version = $runInfo.exe_version; copy_scans = $runInfo.copy_scans; notes = @($runInfo.notes | Where-Object { $_ }) } } else { $null }
  storage = @($storage)
  instances = $instSummaries
  instances_without_log = @($noLogInstances)
  stalls = $stallSummary
  ops = $opRows
  agg = $aggRows
  stages = [ordered]@{ self_ms = $stageRows; batch = $batchRows; batch_recheck = $recheckRows }
  page_load = $plRows
  stage_switch = $swRows
  steps = $stepRows
  cycles = $cycleRows
  files = @($procs | ForEach-Object { $pp = $_; $pp.files | ForEach-Object { [ordered]@{ process = $pp.id; name = $_.name; bytes = $_.bytes; records = $_.records; bad = $_.bad; truncated = $_.truncated } } })
  crash_files = @($crashFiles | ForEach-Object { $_.Name })
  analysis = [ordered]@{ records = $totalRecords; seconds = [Math]::Round($swAll.Elapsed.TotalSeconds, 1); sampled = $sampled; sample_cap = $cap; powershell = $PSVersionTable.PSVersion.ToString() }
}

# ---------------------------------------------------------------------------
# report.html
# ---------------------------------------------------------------------------

$HtmlBody = New-Object System.Text.StringBuilder
$toc = New-Object System.Collections.Generic.List[string]
function Add-Html([string]$s) { [void]$script:HtmlBody.Append($s) }
function Add-Section([string]$id, [string]$title) {
  $script:toc.Add('<a href="#' + $id + '">' + (ConvertTo-DiagHtml $title) + '</a>')
  Add-Html ('<h2 id="' + $id + '">' + (ConvertTo-DiagHtml $title) + '</h2>')
}
function F1($v) { return Format-DiagNumber $v 1 }
function F0($v) { return Format-DiagNumber $v 0 }
function FMs($v) { return Format-DiagMs $v }
function Esc($v) { return ConvertTo-DiagHtml $v }

# 1. Verdict
Add-Section 'verdict' 'Verdict'
if ($partialNote) { Add-Html ('<div class="hint"><b>' + (Esc $partialNote) + '</b></div>') }
Add-Html '<div class="verdict">'
Add-Html ('<div class="overall">' + (Get-DiagStatusBadge $overall) + '<span>Overall, from ' + $checks.Count + ' checks. Each line states what was measured; thresholds are listed with it.</span></div>')
$vr = @()
foreach ($c in $checks) {
  $det = ''
  if ($c.details.Count -gt 0) {
    $det = '<details><summary>' + $c.details.Count + ' detail line(s)</summary><ul>' + (($c.details | ForEach-Object { '<li>' + (Esc $_) + '</li>' }) -join '') + '</ul></details>'
  }
  $ruleHtml = ''
  if ($c.rule) { $ruleHtml = '<div class="muted">' + (Esc $c.rule) + '</div>' }
  $vr += , @(@{ html = (Get-DiagStatusBadge $c.status) }, @{ html = '<b>' + (Esc $c.name) + '</b>' }, @{ html = (Esc $c.summary) + $ruleHtml + $det })
}
Add-Html (New-DiagHtmlTable -Headers @('Status', 'Check', 'Numbers') -Rows $vr)
Add-Html '</div>'
Add-Html '<p class="note">These checks measure; they do not prove. A clean result means nothing above the thresholds happened during this run on this PC. Stalls caused by slow storage or antivirus are real findings: the operator experiences them the same way.</p>'

# 2. Environment
Add-Section 'environment' 'Environment'
$kv = @()
if ($label) { $kv += , @('Label', $label) }
if ($summary.computer) { $kv += , @('Computer', $summary.computer) }
if ($firstStart) {
  $kv += , @('Application', ([string]$firstStart.app + ' ' + [string]$firstStart.version + ' (Qt ' + [string]$firstStart.qt + ')' + $(if ($firstStart.portable) { ', portable' } else { '' })))
  $kv += , @('Executable', [string]$firstStart.exe)
  $kv += , @('Diagnostics level', [string]$firstStart.level)
  $kv += , @('OS', ([string]$firstStart.os + ' (' + [string]$firstStart.os_kernel + ')'))
  $kv += , @('CPU', ([string]$firstStart.cpu + ', ' + [string]$firstStart.cores + ' logical processors'))
  $kv += , @('RAM', (Format-DiagMB $firstStart.ram_mb))
  $kv += , @('Log folder', [string]$firstStart.log_dir)
}
if ($firstSettings -and $null -ne $firstSettings.batch_threads) {
  # batch_threads is the effective count; batch_threads_setting (newer builds)
  # the stored value it came from. Older builds wrote only the stored value.
  if ($null -ne $firstSettings.batch_threads_setting) { $bt = [string]$firstSettings.batch_threads + ' effective (setting ' + [string]$firstSettings.batch_threads_setting + ')' }
  else { $bt = [string]$firstSettings.batch_threads + ' (stored setting; this build does not log the effective count)' }
  if ($firstSettings.worker_thread_priority) { $bt += ', worker thread priority ' + [string]$firstSettings.worker_thread_priority }
  $kv += , @('Batch threads', $bt)
}
if ($firstGenerate) {
  $gt = [string]$firstGenerate.count + ' x ' + [string]$firstGenerate.kind + ' @ ' + [string]$firstGenerate.dpi + ' dpi'
  $gc = Format-TiffCompression $firstGenerate.compression
  if ($gc) { $gt += ', TIFF compression ' + $gc }
  if ($null -ne $firstGenerate.dur) { $gt += ', generated in ' + (FMs $firstGenerate.dur) }
  $kv += , @('Generated pages', $gt)
}
if ($runInfo) {
  $kv += , @('Run started', (Format-DiagWhen $runInfo.started))
  $kv += , @('Run ended', (Format-DiagWhen $runInfo.ended))
  if ($runInfo.params) {
    $pp = $runInfo.params
    $kv += , @('Scenario', ('instances ' + [string]$pp.Instances + ', cycles ' + [string]$pp.Cycles + ', mode ' + [string]$pp.Mode + ', pages ' + [string]$pp.Pages + ', nav pages ' + [string]$pp.NavPages))
    if ($pp.Scans) { $kv += , @('Scans', ([string]$pp.Scans + $(if ($pp.CopyScansLocal) { ' (copied locally first)' } else { ' (read in place)' }))) }
    elseif ($pp.Synthetic) { $kv += , @('Scans', ('synthetic: ' + [string]$pp.Synthetic + ' x ' + [string]$pp.SyntheticKind + ' @ ' + [string]$pp.SyntheticDpi + ' dpi')) }
    if ($pp.UseOperatorSettings) { $kv += , @('Settings', 'operator settings copied from ' + [string]$runInfo.operator_settings) }
  }
  if ($runInfo.copy_scans) { $kv += , @('Scan copy', ([string]$runInfo.copy_scans.files + ' files, ' + (Format-DiagMB $runInfo.copy_scans.mb) + ' in ' + (F1 $runInfo.copy_scans.seconds) + ' s = ' + (F1 $runInfo.copy_scans.mb_per_s) + ' MB/s')) }
}
$kv += , @('Analysed', ([string]$totalRecords + ' records from ' + $perfFiles.Count + ' file(s) in ' + (F1 $swAll.Elapsed.TotalSeconds) + ' s'))
Add-Html '<div class="kv">'
foreach ($row in $kv) { Add-Html ('<div>' + (Esc $row[0]) + '</div><div>' + (Esc $row[1]) + '</div>') }
Add-Html '</div>'

if ($inventory) {
  Add-Html '<h3>Machine inventory</h3>'
  $ir = @()
  if ($inventory.os) { $ir += , @('Windows', ([string]$inventory.os.caption + ' ' + [string]$inventory.os.display_version + ' build ' + [string]$inventory.os.build + '.' + [string]$inventory.os.ubr + ', up ' + [string]$inventory.os.uptime_hours + ' h')) }
  if ($inventory.system) { $ir += , @('Model', ([string]$inventory.system.manufacturer + ' ' + [string]$inventory.system.model + ', RAM ' + (Format-DiagMB $inventory.system.ram_mb))) }
  foreach ($c in @($inventory.cpu)) { if ($c) { $ir += , @('CPU', ([string]$c.name + ' - ' + [string]$c.cores + ' cores / ' + [string]$c.logical + ' threads')) } }
  if ($inventory.os) { $ir += , @('Memory at collection', ('free ' + (Format-DiagMB $inventory.os.free_ram_mb) + ' of ' + (Format-DiagMB $inventory.os.total_ram_mb) + '; commit free ' + (Format-DiagMB $inventory.os.commit_free_mb) + ' of ' + (Format-DiagMB $inventory.os.commit_limit_mb))) }
  foreach ($d in @($inventory.physical_disks)) { if ($d) { $ir += , @('Disk', ([string]$d.name + ' - ' + [string]$d.media_type + ' ' + [string]$d.bus_type + ', ' + [string]$d.size_gb + ' GB')) } }
  foreach ($pth in @($inventory.paths)) {
    if (-not $pth) { continue }
    $desc = ''
    if ($pth.network) { $desc = 'NETWORK (' + [string]$pth.target + ')' }
    else {
      $desc = [string]$pth.drive_type + ' disk'
      if ($pth.media_type) { $desc += ' (' + [string]$pth.media_type + ')' }
    }
    $desc += ', ' + [string]$pth.fs + ', free ' + [string]$pth.free_gb + ' GB'
    if ($null -ne $pth.files_top) { $desc += ', ' + [string]$pth.files_top + ' files' + $(if (-not $pth.count_complete) { '+' } else { '' }) + ' (' + [string]$pth.images_top + ' images)' }
    if (-not $pth.ascii) { $desc += ', path has non-ASCII characters' }
    $ir += , @(('Path ' + [string]$pth.path), $desc)
  }
  foreach ($a in @($inventory.network_adapters)) { if ($a) { $ir += , @('Network', ([string]$a.name + ' - ' + [string]$a.description + ', ' + [string]$a.link_speed)) } }
  if ($inventory.power) { $ir += , @('Power plan', ([string]$inventory.power.name + $(if ($inventory.power.known_as -and $inventory.power.known_as -ne $inventory.power.name) { ' (' + [string]$inventory.power.known_as + ')' } else { '' }) + $(if ($inventory.power.has_battery) { ', has battery' } else { '' }))) }
  if ($inventory.defender) {
    $df = $inventory.defender
    $txt = 'real-time ' + $(if ($df.realtime) { 'ON' } else { 'off' })
    if ($df.exclusions_readable) { $txt += '; exclusions: ' + ((@($df.exclusion_path) + @($df.exclusion_process)) -join ', ') } else { $txt += '; exclusions not readable without administrator rights' }
    $ir += , @('Defender', $txt)
  }
  foreach ($a in @($inventory.antivirus_products)) { if ($a) { $ir += , @('Antivirus', ([string]$a.name + $(if ($a.enabled) { ' (enabled)' } else { ' (disabled)' }))) } }
  foreach ($v in @($inventory.display_adapters)) { if ($v) { $ir += , @('Display adapter', ([string]$v.name + ', driver ' + [string]$v.driver_version + ' (' + [string]$v.driver_date + '), ' + [string]$v.resolution)) } }
  if ($inventory.profile -and $inventory.profile.appdata) {
    $ap = $inventory.profile.appdata
    $ir += , @('User profile (settings INI)', ([string]$ap.path + $(if ($ap.network -or $ap.unc) { ' - ON A NETWORK DRIVE: every settings write is a network round trip' } else { ' - local' })))
  }
  if ($inventory.fonts) { $ir += , @('Installed fonts', ([string]$inventory.fonts.registered_machine + ' machine-wide, ' + [string]$inventory.fonts.registered_user + ' per-user (Qt enumerates all of them the first time it draws text)')) }
  if ($inventory.temp) { $ir += , @('%TEMP%', ([string]$inventory.temp.files + $(if (-not $inventory.temp.complete) { '+ (counting stopped)' } else { '' }) + ' files, ' + (Format-DiagBytes $inventory.temp.bytes))) }
  foreach ($pr in @($inventory.processes)) { if ($pr) { $ir += , @(('Process: ' + [string]$pr.category), ([string]$pr.name + ' x' + [string]$pr.count + ', ' + [string]$pr.ws_mb + ' MB')) } }
  foreach ($pf in @($inventory.pagefile)) { if ($pf) { $ir += , @('Page file', ([string]$pf.name + ' ' + [string]$pf.allocated_mb + ' MB, peak use ' + [string]$pf.peak_mb + ' MB')) } }
  if ($inventory.errors -and @($inventory.errors).Count -gt 0) { $ir += , @('Inventory gaps', (@($inventory.errors) -join '; ')) }
  Add-Html (New-DiagHtmlTable -Headers @('Item', 'Value') -Rows $ir)
}

$screensShown = $false
foreach ($p in $procs) {
  if ($p.screens.Count -gt 0) {
    $sr = @()
    foreach ($s in $p.screens) { $sr += , @([string]$s.index, [string]$s.name, $(if ($s.primary) { 'yes' } else { '' }), ([string]$s.w + 'x' + [string]$s.h), (Format-DiagNumber $s.dpr 2), (F0 $s.logical_dpi), (F0 $s.physical_dpi), (F0 $s.refresh_hz)) }
    Add-Html '<h3>Screens (as the application saw them)</h3>'
    Add-Html (New-DiagHtmlTable -Headers @('#', 'Name', 'Primary', 'Logical size', 'Pixel ratio', 'Logical DPI', 'Physical DPI', 'Hz') -Rows $sr -NumericColumns @(4, 5, 6, 7))
    $screensShown = $true
    break
  }
}
if ($firstSettings) {
  $sr = @()
  $m = ConvertTo-PlainMap $firstSettings
  $settingLabels = @{ batch_threads_setting = 'batch_threads_setting (stored value)'; worker_thread_priority = 'worker_thread_priority (batch worker threads)' }
  if ($m.Contains('batch_threads_setting')) { $settingLabels['batch_threads'] = 'batch_threads (effective)' }
  foreach ($k in $m.Keys) {
    if ($k -in @('t', 'ev', 'th', 'tid')) { continue }
    $vals = @{}
    foreach ($p in $procs) { if ($p.settings) { $vals[[string]$p.settings.$k] = $true } }
    $cell = [string]$m[$k]
    if ($m[$k] -is [bool]) { $cell = $cell.ToLowerInvariant() }
    if ($k -like 'tiff_*_compression' -and $vals.Count -le 1) { $tc = Format-TiffCompression $m[$k]; if ($tc) { $cell = $tc } }
    $lbl = $k
    if ($settingLabels.ContainsKey($k)) { $lbl = $settingLabels[$k] }
    if ($vals.Count -gt 1) { $sr += , @($lbl, @{ text = ($vals.Keys -join ' | '); cls = 'diff' }) } else { $sr += , @($lbl, $cell) }
  }
  Add-Html '<h3>Application settings</h3>'
  Add-Html (New-DiagHtmlTable -Headers @('Setting', 'Value') -Rows $sr)
}
if (@($storage).Count -gt 0) {
  Add-Html '<h3>Storage benchmark</h3>'
  Add-Html '<p class="muted">Measured by the script, outside the application, in a temporary folder that is removed afterwards (a cleanup error below names any folder left behind). Times in ms; p50 is the median, p95 the value 95% of operations stayed under.</p>'
  $sr = @()
  foreach ($s in @($storage)) {
    if (-not $s) { continue }
    $t = $s.tests
    $cellsFor = {
      param($x)
      if ($null -eq $x) { return '-' }
      return ((Format-DiagNumber $x.p50 2) + ' / ' + (Format-DiagNumber $x.p95 2))
    }
    $modeText = [string]$s.mode
    if ($s.PSObject.Properties['network'] -and $null -ne $s.network) { $modeText += $(if ($s.network) { ', network' } else { ', local' }) }
    $sr += , @(
      ([string]$s.label), ([string]$s.path), $modeText,
      (& $cellsFor $t.small_create), (& $cellsFor $t.stat), (& $cellsFor $t.durable_1mb), (& $cellsFor $t.rename),
      $(if ($t.seq_write) { F1 $t.seq_write.mb_per_s } else { '-' }),
      $(if ($t.seq_read) { (F1 $t.seq_read.mb_per_s) + $(if ($t.seq_read.cached) { ' (cache)' } else { '' }) } else { '-' })
    )
  }
  Add-Html (New-DiagHtmlTable -Headers @('Location', 'Path', 'Mode', 'Create 4 KB p50/p95', 'Stat p50/p95', 'Durable 1 MB p50/p95', 'Rename p50/p95', 'Seq write MB/s', 'Seq read MB/s') -Rows $sr -NumericColumns @(3, 4, 5, 6, 7, 8))
  foreach ($s in @($storage)) {
    if (-not $s) { continue }
    foreach ($h in (Get-StorageHints $s)) { Add-Html ('<div class="hint"><b>' + (Esc $s.label) + ':</b> ' + (Esc $h) + '</div>') }
    foreach ($e in @($s.errors)) { if ($e) { Add-Html ('<div class="hint"><b>' + (Esc $s.label) + ' error:</b> ' + (Esc $e) + '</div>') } }
    foreach ($nt in @($s.notes)) { if ($nt) { Add-Html ('<div class="note"><b>' + (Esc $s.label) + ':</b> ' + (Esc $nt) + '</div>') } }
  }
  Add-Html '<p class="muted">Rough guide: on a local SSD a durable 1 MB write takes 1-5 ms, a small create under 1 ms, a stat under 0.1 ms. Over 20 ms for the durable write suggests a network share or a slow disk; over 1 ms per stat means network latency on a share, and on a local disk an antivirus/EDR or other file-system filter driver.</p>'
}

# 3. Stalls
Add-Section 'stalls' 'GUI stalls'
if ($allStalls.Count -eq 0) {
  Add-Html '<p>No stalls were recorded: the GUI thread never stopped answering for longer than the stall threshold (250 ms by default).</p>'
} else {
  Add-Html ('<p>' + $allStalls.Count + ' stall(s). <span class="muted">A stall is the GUI thread not answering its 50 ms heartbeat; the recorded duration overstates it by up to 50 ms. "Open operations" are the instrumented operations running on the GUI thread, outermost first; the stack (innermost first) is captured once a stall passes 1 s. A duration marked &gt;= is a stall the log never saw end (the last progress record the watchdog wrote while it was going): it lasted at least that long.</span></p>')
  Add-Html '<div class="grid2">'
  $hv = [double[]]$hist
  Add-Html (New-DiagSvgBarChart -Labels $histLabels -Values $hv -Title 'Stall durations (count)' -Color $Palette[0] -Titles @($histLabels | ForEach-Object { $_ }))
  $marks = @()
  $legend = @()
  foreach ($p in $procs) {
    if ($p.stalls.Count -eq 0) { continue }
    $legend += , @{ Name = $p.id; Color = $p.color }
    foreach ($s in $p.stalls) {
      $at = [double]$s.at
      if ($null -eq $s.at) { $at = [double]$s.t - [double]$s.dur }
      $inner = @($s.ops_last | Where-Object { $_ })
      if ($inner.Count -eq 0) { $inner = @($s.ops | Where-Object { $_ }) }
      $what = '(none)'
      if ($inner.Count -gt 0) { $what = [string]$inner[$inner.Count - 1] }
      $marks += , @{ X = ($at - $p.first_t) / 60000.0; Y = [double]$s.dur / 1000.0; Color = $p.color; Title = ($p.id + ', ' + (Get-StallWhen $p $s) + ': ' + $(if ($s.unended) { '>= ' } else { '' }) + (Format-DiagMs $s.dur) + ' in ' + $what + ' [' + $StallClassText[(Get-StallClass $s)] + ']') }
    }
  }
  $legendArg = @()
  if ($legend.Count -gt 1) { $legendArg = $legend }
  Add-Html (New-DiagSvgLineChart -Series @() -Marks $marks -Legend $legendArg -Title 'Stalls over time (seconds; hover a dot for details)' -XLabel 'minutes since the process started' -Width 620 -Height 220 -ZeroBased)
  Add-Html '</div>'
  $maxMs = 0.0
  foreach ($k in $byOpInner.Keys) { if ($byOpInner[$k].ms -gt $maxMs) { $maxMs = $byOpInner[$k].ms } }
  $rows = @()
  foreach ($e in $stallSummary.by_innermost_op) { $rows += , @($e.op, [string]$e.n, @{ html = (Esc (FMs $e.ms)) + (New-DiagInlineBar -Value $e.ms -Max $maxMs -Color $Palette[1]) }, (FMs $e.max)) }
  Add-Html ('<h3>Which operation the GUI thread was in (innermost open operation)</h3><p class="muted">Over ' + $(if ($rankAll) { 'all stalls (none was counted in the verdict)' } else { 'the ' + $rankStalls.Count + ' stall(s) counted in the verdict' }) + '.</p>')
  Add-Html (New-DiagHtmlTable -Headers @('Operation', 'Stalls', 'Stalled time', 'Longest') -Rows $rows -NumericColumns @(1, 2, 3))
  $rows = @()
  $maxMs = 0.0
  foreach ($k in $byOpAny.Keys) { if ($byOpAny[$k].ms -gt $maxMs) { $maxMs = $byOpAny[$k].ms } }
  foreach ($e in $stallSummary.by_any_op) { $rows += , @($e.op, [string]$e.n, @{ html = (Esc (FMs $e.ms)) + (New-DiagInlineBar -Value $e.ms -Max $maxMs -Color $Palette[0]) }, (FMs $e.max)) }
  Add-Html '<details><summary>Any open operation, parents included</summary>'
  Add-Html (New-DiagHtmlTable -Headers @('Operation', 'Stalls', 'Stalled time', 'Longest') -Rows $rows -NumericColumns @(1, 2, 3))
  Add-Html '</details>'
  Add-Html ('<h3>Stalls, longest first' + $(if ($allStalls.Count -gt $MaxStallRows) { ' (top ' + $MaxStallRows + ' of ' + $allStalls.Count + '; all in stalls.csv)' } else { '' }) + '</h3>')
  $rows = @()
  foreach ($x in ($allStalls | Select-Object -First $MaxStallRows)) {
    $s = $x.s
    $durCell = FMs $x.dur
    if ($s.unended) {
      $durCell = '>= ' + $durCell + $(if ($s.log_ends) { ' (log ends during it)' } else { ' (no end record)' })
    } elseif ($s.ongoing) { $durCell += ' (ongoing)' }
    $cls = ''
    if ($x.dur -gt $Thresholds.StallFailMs -or ($s.unended -and $x.dur -ge $Thresholds.StallFailMs)) { $cls = 'bad' } elseif ($x.dur -gt $Thresholds.StallWarnMs -or ($s.unended -and $x.dur -ge $Thresholds.StallWarnMs)) { $cls = 'hl' }
    $frames = @($s.stack | Select-Object -First 6)
    $stackHtml = ''
    if ($frames.Count -gt 0) { $stackHtml = '<span class="mono">' + (($frames | ForEach-Object { Esc $_ }) -join '<br>') + '</span>' }
    if ($s.dump) { $stackHtml += '<br><span class="muted">dump: ' + (Esc $s.dump) + '</span>' }
    if ($x.cls -ne 'run') { $cls = '' }
    $opsText = @($s.ops | Where-Object { $_ }) -join ' > '
    if (-not $opsText) { $opsText = '(none)' }
    $opsLastText = @($s.ops_last | Where-Object { $_ }) -join ' > '
    if (-not $opsLastText) { $opsLastText = '(none)' }
    $rows += , @($x.proc.id, (Get-StallWhen $x.proc $s), $StallClassText[$x.cls], @{ text = $durCell; cls = $cls }, $opsText, $opsLastText, @{ html = $stackHtml })
  }
  Add-Html (New-DiagHtmlTable -Headers @('Process', 'When', 'Attributed to', 'Duration', 'Open operations', 'Open at the end', 'Top stack frames') -Rows $rows -NumericColumns @(3))
}
$beatRows = @()
foreach ($p in $procs) {
  $b = $p.beat
  if ($b.n -gt 0) {
    $beatRows += , @($p.id, [string]$b.n, (Format-DiagNumber (100.0 * $b.late50 / $b.n) 2), (Format-DiagNumber (100.0 * $b.late100 / $b.n) 2), (Format-DiagNumber (100.0 * $b.late250 / $b.n) 2), [string]$b.late1000, (FMs $b.max_late))
  }
}
if ($beatRows.Count -gt 0) {
  Add-Html '<h3>Heartbeat lateness</h3><p class="muted">Share of 50 ms heartbeats that arrived late. A responsive GUI thread keeps nearly all of them under 50 ms.</p>'
  Add-Html (New-DiagHtmlTable -Headers @('Process', 'Heartbeats', '% > 50 ms late', '% > 100 ms', '% > 250 ms', '> 1 s (count)', 'Max lateness') -Rows $beatRows -NumericColumns @(1, 2, 3, 4, 5, 6))
}

# 4. Operation timings
Add-Section 'ops' 'Operation timings'
$basic = @($procs | Where-Object { $_.level -eq 'basic' }).Count -gt 0
if ($basic) {
  Add-Html '<p class="note">These logs are (at least partly) at the basic level: individual operation records exist only for slow calls (50 ms or more on the GUI thread, 2 s elsewhere), so the percentiles below describe slow calls only. Exceptions, in current builds: stage.* records are written for every page, and task.run for every task that failed. The aggregate table counts every call.</p>'
}
if ($sampled) { Add-Html ('<p class="note">More than ' + $cap + ' records for some operations: their percentiles come from a uniform random sample of ' + $cap + ' values. Counts, totals and maxima are exact.</p>') }
if ($opRows.Count -gt 0) {
  $rows = @()
  foreach ($o in $opRows) {
    $hl = ($o.th -eq 'gui' -and $o.max -ge $Thresholds.GuiSlowOpMs)
    $mcell = FMs $o.max
    $rows += , @($(if ($hl) { @{ text = $o.name; cls = 'hl' } } else { $o.name }), $o.th, [string]$o.n, (FMs $o.sum), (Format-DiagNumber $o.mean 1), (Format-DiagNumber $o.p50 1), (Format-DiagNumber $o.p95 1), (Format-DiagNumber $o.p99 1), $(if ($hl) { @{ text = $mcell; cls = 'hl' } } else { $mcell }), $(if ($o.sampled) { 'sampled' } else { '' }))
  }
  Add-Html '<p class="muted">From individual op records, by thread. Highlighted: GUI-thread operations that took 250 ms or more at least once (each such call froze the window for that long). Times in ms unless marked.</p>'
  Add-Html (New-DiagHtmlTable -Headers @('Operation', 'Thread', 'Calls', 'Total', 'Mean', 'p50', 'p95', 'p99', 'Max', '') -Rows $rows -NumericColumns @(2, 3, 4, 5, 6, 7, 8))
} else {
  Add-Html '<p class="muted">No individual operation records.</p>'
}
if ($aggRows.Count -gt 0) {
  $rows = @()
  foreach ($a in $aggRows) { $rows += , @($a.name, [string]$a.n, (FMs $a.sum), (Format-DiagNumber $a.mean 2), (FMs $a.max)) }
  Add-Html '<h3>Complete accounting (aggregates)</h3><p class="muted">Every call of every instrumented operation, whether or not it was written individually. Mean in ms.</p>'
  Add-Html (New-DiagHtmlTable -Headers @('Operation', 'Calls', 'Total', 'Mean', 'Max') -Rows $rows -NumericColumns @(1, 2, 3, 4))
}

# 5. Stage throughput
Add-Section 'stages' 'Stage throughput'
if ($stageRows.Count -gt 0) {
  Add-Html '<p class="muted">Own time of each processing stage per page (task): the stage''s duration minus the stage it hands on to. Covers interactive and batch tasks. Times in ms.</p>'
  $partial = @($stageRows | Where-Object { -not $_.complete })
  if ($basic -and $partial.Count -eq 0) {
    Add-Html '<p class="note">Basic-level logs: stage records are written for every page whatever its duration, so this table is complete even though other operations are recorded only when slow.</p>'
  }
  if ($partial.Count -gt 0) {
    Add-Html ('<p class="note">Not every stage call has a record here (' + (Esc (($partial | ForEach-Object { $_.name + ' ' + $_.n + ' of ' + $_.calls_all }) -join ', ')) + ' calls, from the aggregates): basic-level logs from builds that wrote stage records only when slow, or records dropped. Those rows describe the recorded, mostly slow, pages only.</p>')
  }
  $rows = @()
  foreach ($s in $stageRows) {
    $pages = [string]$s.n
    if (-not $s.complete) { $pages = @{ text = ([string]$s.n + ' of ' + [string]$s.calls_all); cls = 'hl' } }
    $rows += , @([string]$s.stage, $s.name, $pages, (F1 $s.mean), (F1 $s.p50), (F1 $s.p95), (FMs $s.max))
  }
  Add-Html (New-DiagHtmlTable -Headers @('#', 'Stage', 'Pages', 'Mean', 'p50', 'p95', 'Max') -Rows $rows -NumericColumns @(2, 3, 4, 5, 6))
}
if ($batchRows.Count -gt 0 -or $recheckRows.Count -gt 0) {
  Add-Html '<h3>Batch processing (stress scenario)</h3>'
  if ($batchRows.Count -gt 0) {
    Add-Html (New-DiagSvgBarChart -Labels @($batchRows | ForEach-Object { [string]$_.name }) -Values ([double[]]@($batchRows | ForEach-Object { [double]$_.mean })) -Title 'Mean seconds per page, by stage' -Color $Palette[0] -ValueLabels @($batchRows | ForEach-Object { Format-DiagNumber $_.mean 2 }) -Width 700)
  }
  $rows = @()
  foreach ($b in $batchRows) { $rows += , @([string]$b.stage, $b.name, [string]$b.runs, [string]$b.pages, (Format-DiagNumber $b.mean 2), (Format-DiagNumber $b.min 2), (Format-DiagNumber $b.max 2), [string]$b.failed) }
  foreach ($b in $recheckRows) { $rows += , @([string]$b.stage, $b.name, [string]$b.runs, [string]$b.pages, (Format-DiagNumber $b.mean 2), (Format-DiagNumber $b.min 2), (Format-DiagNumber $b.max 2), [string]$b.failed) }
  Add-Html (New-DiagHtmlTable -Headers @('#', 'Stage', 'Runs', 'Pages', 'Mean s/page', 'Min', 'Max', 'Not ok') -Rows $rows -NumericColumns @(2, 3, 4, 5, 6, 7))
  if ($recheckRows.Count -gt 0) {
    Add-Html '<p class="muted">"Re-check of finished output": in reopen mode every cycle after the first runs Output again over pages whose output is already up to date, so the application only checks that it is. That is timed separately and left out of the stage means, the chart, the per-cycle highlighting and the comparison between reports.</p>'
  }
  # Matrix: cycles x stages, to spot a slowdown that builds up over cycles.
  # Re-check runs get their own column and are never highlighted: comparing
  # them with real processing would flag every first cycle.
  $stagesSeen = @($batchRows | ForEach-Object { [int]$_.stage })
  $recheckSeen = @($recheckRows | ForEach-Object { [int]$_.stage })
  $minBy = @{}
  foreach ($b in $batchRows) { $minBy[[int]$b.stage] = [double]$b.min }
  $rows = @()
  foreach ($p in $procs) {
    $byCycle = @{}
    $reByCycle = @{}
    foreach ($b in $p.batches) {
      $c = [int]$b.cycle
      if (-not $byCycle.ContainsKey($c)) { $byCycle[$c] = @{} }
      $byCycle[$c][[int]$b.stage] = [double]$b.sec_per_page
    }
    foreach ($b in $p.rebatches) {
      $c = [int]$b.cycle
      if (-not $byCycle.ContainsKey($c)) { $byCycle[$c] = @{} }
      if (-not $reByCycle.ContainsKey($c)) { $reByCycle[$c] = @{} }
      $reByCycle[$c][[int]$b.stage] = [double]$b.sec_per_page
    }
    foreach ($c in ($byCycle.Keys | Sort-Object)) {
      $row = @($p.id, [string]$c)
      foreach ($sk in $stagesSeen) {
        $v = $byCycle[$c][$sk]
        if ($null -eq $v) { $row += '-' }
        elseif ($minBy[$sk] -gt 0 -and $v -gt 1.5 * $minBy[$sk]) { $row += @{ text = (Format-DiagNumber $v 2); cls = 'hl' } }
        else { $row += (Format-DiagNumber $v 2) }
      }
      foreach ($sk in $recheckSeen) {
        $v = $null
        if ($reByCycle.ContainsKey($c)) { $v = $reByCycle[$c][$sk] }
        if ($null -eq $v) { $row += '-' } else { $row += (Format-DiagNumber $v 2) }
      }
      $rows += , $row
    }
  }
  $hdr = @('Process', 'Cycle') + @($batchRows | ForEach-Object { [string]$_.name }) + @($recheckRows | ForEach-Object { [string]$_.name })
  Add-Html '<p class="muted">Seconds per page by cycle. Highlighted: more than 1.5 times the fastest run of that stage (re-check columns are not highlighted).</p>'
  Add-Html (New-DiagHtmlTable -Headers $hdr -Rows $rows -NumericColumns @(2..($hdr.Count - 1)))
}
if ($stageRows.Count -eq 0 -and $batchRows.Count -eq 0 -and $recheckRows.Count -eq 0) { Add-Html '<p class="muted">No stage records.</p>' }

# 6. Page loads
if ($plRows.Count -gt 0 -or $swRows.Count -gt 0 -or $stepRows.Count -gt 0) {
  Add-Section 'pageload' 'Page loads and stage switches'
  if ($plRows.Count -gt 0) {
    Add-Html '<p class="muted">From selecting a page until it is on screen.</p>'
    $rows = @()
    foreach ($r in $plRows) { $rows += , @([string]$r.stage, $r.name, [string]$r.n, (FMs $r.p50), (FMs $r.p95), (FMs $r.max), $r.outcomes) }
    Add-Html (New-DiagHtmlTable -Headers @('#', 'Stage', 'Loads', 'p50', 'p95', 'Max', 'Outcomes') -Rows $rows -NumericColumns @(2, 3, 4, 5))
  }
  if ($swRows.Count -gt 0) {
    Add-Html '<h3>Stage switches</h3>'
    $rows = @()
    foreach ($r in $swRows) {
      $fn = if ($r.from -ge 0 -and $r.from -lt $StageNames.Count) { $StageNames[$r.from] } else { [string]$r.from }
      $tn = if ($r.to -ge 0 -and $r.to -lt $StageNames.Count) { $StageNames[$r.to] } else { [string]$r.to }
      $rows += , @(($fn + ' -> ' + $tn), [string]$r.n, (FMs $r.p50), (FMs $r.p95), (FMs $r.max))
    }
    Add-Html (New-DiagHtmlTable -Headers @('Switch', 'Count', 'p50', 'p95', 'Max') -Rows $rows -NumericColumns @(1, 2, 3, 4))
  }
  if ($stepRows.Count -gt 0) {
    Add-Html '<h3>Scenario steps</h3>'
    $rows = @()
    foreach ($r in $stepRows) { $rows += , @($r.step, [string]$r.n, $(if ($r.skips_by_design) { 'skipped by design' } else { [string]$r.failed }), (FMs $r.p50), (FMs $r.p95), (FMs $r.max)) }
    Add-Html (New-DiagHtmlTable -Headers @('Step', 'Count', 'Not ok', 'p50', 'p95', 'Max') -Rows $rows -NumericColumns @(1, 2, 3, 4, 5))
    if (@($stepRows | Where-Object { ([string]$_.step).StartsWith('autosave: ') }).Count -gt 0) {
      Add-Html '<p class="muted">Autosaves by branch, i.e. what the autosave decided to do: project_file, snapshot and unsaved_session write something and are "not ok" when nothing was written; batch_skip, guard_skip, disabled and busy write nothing by design.</p>'
    }
  }
}

# 7. Resources
Add-Section 'resources' 'Resources over time'
$chartProcs = @($procs | Where-Object { $_.series['private_mb'].X.Count -gt 0 } | Sort-Object { $_.duration_ms } -Descending | Select-Object -First 8 | Sort-Object { $_.id })
if ($chartProcs.Count -eq 0) {
  Add-Html '<p class="muted">No resource samples.</p>'
} else {
  if ($chartProcs.Count -lt @($procs | Where-Object { $_.series['private_mb'].X.Count -gt 0 }).Count) { Add-Html '<p class="note">Charts show the 8 longest-running processes.</p>' }
  Add-Html '<p class="muted">Dashed lines: start of a stress cycle. Dots: the settled sample taken at the end of each cycle (the one leak detection uses); hover for values.</p>'
  $metrics = @(
    @{ k = 'private_mb'; t = 'Private memory (commit), MB'; mode = 'max' },
    @{ k = 'ws_mb'; t = 'Working set, MB'; mode = 'max' },
    @{ k = 'handles'; t = 'Kernel handles'; mode = 'max' },
    @{ k = 'gdi'; t = 'GDI objects'; mode = 'max' },
    @{ k = 'user'; t = 'USER objects'; mode = 'max' },
    @{ k = 'threads'; t = 'Threads'; mode = 'max' },
    @{ k = 'cpu_pct'; t = 'CPU, % of the whole machine (averaged per point)'; mode = 'mean'; points = 250 }
  )
  foreach ($m in $metrics) {
    $ser = @(); $vl = @(); $marks = @()
    foreach ($p in $chartProcs) {
      $sd = $p.series[$m.k]
      if ($sd.X.Count -eq 0) { continue }
      $ser += , @{ Name = $p.id; Color = $p.color; X = $sd.X.ToArray(); Y = $sd.Y.ToArray() }
      foreach ($cb in $p.cycle_begin_t) { $vl += , @{ X = $cb / 60000.0; Color = $p.color; Label = $p.id + ' cycle start' } }
      if ($m.k -ne 'cpu_pct') {
        foreach ($c in $p.cycle_end) {
          if ($null -ne $c.($m.k)) { $marks += , @{ X = [double]$c.t / 60000.0; Y = [double]$c.($m.k); Color = $p.color; Title = ($p.id + ' cycle ' + [string]$c.cycle + ' end: ' + (Format-DiagNumber $c.($m.k) 1)) } }
        }
      }
    }
    if ($ser.Count -eq 0) { continue }
    $maxPoints = 900
    if ($m.points) { $maxPoints = $m.points }
    Add-Html (New-DiagSvgLineChart -Series $ser -Title $m.t -VLines $vl -Marks $marks -Mode $m.mode -MaxPoints $maxPoints)
  }
  # System memory
  $sysRows = @()
  foreach ($p in $procs) {
    $av = $p.series['sys_avail_mb'].Y; $ld = $p.series['sys_load_pct'].Y
    if ($av.Count -eq 0) { continue }
    $minAv = ($av | Measure-Object -Minimum).Minimum
    $maxLd = if ($ld.Count -gt 0) { ($ld | Measure-Object -Maximum).Maximum } else { $null }
    $sysRows += , @($p.id, (Format-DiagMB $minAv), (F0 $maxLd), (Format-DiagMB $p.peak_ws_mb), (F0 $p.log_queue_max))
  }
  if ($sysRows.Count -gt 0) {
    Add-Html '<h3>Machine memory pressure</h3>'
    Add-Html (New-DiagHtmlTable -Headers @('Process', 'Lowest available RAM', 'Highest memory load %', 'Peak working set', 'Max log backlog') -Rows $sysRows -NumericColumns @(1, 2, 3, 4))
  }
}
if ($cycleRows.Count -gt 0) {
  Add-Html '<h3>End-of-cycle samples</h3><p class="muted">Taken after the project was closed and the process settled; cycle 1 is warm-up and not used for the trend.</p>'
  $rows = @()
  foreach ($c in $cycleRows) { $rows += , @($c.instance, [string]$c.cycle, (FMs $c.dur_ms), (F1 $c.private_mb), (F1 $c.ws_mb), (F0 $c.handles), (F0 $c.gdi), (F0 $c.user), (F0 $c.threads), (Format-DiagMB $c.sys_avail_mb)) }
  Add-Html (New-DiagHtmlTable -Headers @('Process', 'Cycle', 'Cycle time', 'Private MB', 'WS MB', 'Handles', 'GDI', 'USER', 'Threads', 'Avail. RAM') -Rows $rows -NumericColumns @(1, 2, 3, 4, 5, 6, 7, 8, 9))
  $rows = @()
  foreach ($p in $procs) {
    if ($p.cycle_end.Count -eq 0) { continue }
    $row = @($p.id)
    $used = 0
    foreach ($e in $p.leak) {
      if ($e.n -gt $used) { $used = $e.n }
      if ($null -eq $e.slope) { $row += @{ text = '-'; cls = '' }; continue }
      $cls = ''
      if ($e.status -eq 'FAIL') { $cls = 'bad' } elseif ($e.status -eq 'WARN') { $cls = 'hl' }
      $row += @{ text = ((Format-DiagNumber $e.slope 2) + ' (r2 ' + (Format-DiagNumber $e.r2 2) + ')'); cls = $cls }
    }
    $row = @($row[0], [string]$used) + $row[1..($row.Count - 1)]
    $rows += , $row
  }
  if ($rows.Count -gt 0) {
    Add-Html '<h3>Growth per cycle (linear fit)</h3><p class="muted">Change per cycle from a straight-line fit over the end-of-cycle samples after the warm-up cycle; r2 near 1 means steady growth, near 0 means noise. Highlighted: WARN; red: FAIL.</p>'
    Add-Html (New-DiagHtmlTable -Headers @('Process', 'Cycles used', 'Private MB', 'Handles', 'GDI', 'USER', 'Threads') -Rows $rows -NumericColumns @(1, 2, 3, 4, 5, 6))
  }
}
$trendRows = @($procs | Where-Object { $_.trend -and $_.cycle_end.Count -eq 0 } | ForEach-Object { , @($_.id, (Format-DiagNumber $_.trend.mb_per_hour 1), (Format-DiagNumber $_.trend.r2 2), (Format-DiagNumber $_.trend.hours 1)) })
if ($trendRows.Count -gt 0) {
  Add-Html '<h3>Private memory trend per session (informational)</h3><p class="muted">Straight-line fit over the periodic samples after the first 10 minutes. Normal use grows and shrinks memory with the project open, so this is a pointer, not a leak verdict.</p>'
  Add-Html (New-DiagHtmlTable -Headers @('Session', 'MB per hour', 'r2', 'Hours') -Rows $trendRows -NumericColumns @(1, 2, 3))
}
$runnerCsv = Join-Path $root 'runner-samples.csv'
if (Test-Path -LiteralPath $runnerCsv) {
  try {
    $rs = @(Import-Csv -LiteralPath $runnerCsv)
    if ($rs.Count -gt 0) {
      $ser = @()
      $ii = 0
      foreach ($grp in ($rs | Group-Object instance)) {
        $xs = [double[]]@($grp.Group | ForEach-Object { [double]::Parse($_.elapsed_s, $Inv) / 60.0 })
        $ys = [double[]]@($grp.Group | ForEach-Object { if ($_.private_mb) { [double]::Parse($_.private_mb, $Inv) } else { 0 } })
        $ser += , @{ Name = ('inst' + $grp.Name); Color = $Palette[$ii % $Palette.Count]; X = $xs; Y = $ys }
        $ii++
      }
      Add-Html '<h3>External view (sampled by the test runner every 5 s)</h3><p class="muted">Independent of the application''s own logging; useful when a process died before writing its samples. Minutes since the runner started the instances.</p>'
      Add-Html (New-DiagSvgLineChart -Series $ser -Title 'Private bytes seen by the runner, MB' -XLabel 'minutes since launch')
    }
  } catch { Add-Html ('<p class="muted">runner-samples.csv could not be read: ' + (Esc $_.Exception.Message) + '</p>') }
}

# 8. Disk I/O
$ioRows = @()
foreach ($p in $procs) {
  if ($null -eq $p.io.write_mb -and $null -eq $p.io.read_mb) { continue }
  $bw = @()
  foreach ($k in ($p.io.batch_write_mbps.Keys | Sort-Object)) {
    $v = (Get-SampleStats -Values $p.io.batch_write_mbps[$k]).mean
    $nm = if ([int]$k -ge 0 -and [int]$k -lt $StageNames.Count) { $StageNames[[int]$k] } else { $k }
    $bw += ($nm + ' ' + (Format-DiagNumber $v 1))
  }
  $ioRows += , @($p.id, (Format-DiagMB $p.io.read_mb), (Format-DiagMB $p.io.write_mb), ($bw -join ', '))
}
if ($ioRows.Count -gt 0) {
  Add-Section 'io' 'Disk I/O'
  Add-Html '<p class="muted">Process I/O counters (all files, including reading scans and writing output and caches). Write rate during batch = MB written between the samples bracketing each batch run, per second, averaged by stage.</p>'
  Add-Html (New-DiagHtmlTable -Headers @('Process', 'Read', 'Written', 'Write MB/s during batch, by stage') -Rows $ioRows -NumericColumns @(1, 2))
}

# 9. Per-instance comparison
if ($procs.Count -gt 1) {
  Add-Section 'instances' 'Per-process comparison'
  $rows = @()
  foreach ($x in $instSummaries) {
    $p = $procs | Where-Object { $_.id -eq $x.id } | Select-Object -First 1
    $mem = $x.leak | Where-Object { $_.metric -eq 'private_mb' } | Select-Object -First 1
    $bs = @($x.batch_sec_per_page.Keys | ForEach-Object { $x.batch_sec_per_page[$_] })
    $bsum = 0.0; foreach ($v in $bs) { $bsum += [double]$v }
    $exit = Format-ExitCode $x.exit_code
    $rows += , @($x.id, $exit, (FMs ($x.duration_s * 1000)), [string]$x.cycles_done, [string]$x.stalls.count, (FMs $x.stalls.longest_ms), (Format-DiagNumber $x.stalls.pct_of_run 2),
      $(if ($mem -and $null -ne $mem.slope) { Format-DiagNumber $mem.slope 2 } else { '-' }), (F0 $x.peak_ws_mb), $(if ($bs.Count -gt 0) { Format-DiagNumber $bsum 2 } else { '-' }),
      (FMs $x.page_load.p95), [string]($x.failures + $x.dialogs + $x.page_errors + $x.bad_tasks))
  }
  Add-Html (New-DiagHtmlTable -Headers @('Process', 'Exit', 'Duration', 'Cycles', 'Stalls', 'Longest', '% stalled', 'Private MB/cycle', 'Peak WS MB', 'Batch s/page (all stages)', 'Page load p95', 'Problems') -Rows $rows -NumericColumns @(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11))
}

# 10. Files
Add-Section 'files' 'Files'
$rows = @()
foreach ($p in $procs) {
  foreach ($f in $p.files) {
    $rel = $f.path
    if ($rel.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) { $rel = $rel.Substring($root.Length).TrimStart('\', '/') } elseif ($rel.StartsWith($rootListed, [System.StringComparison]::OrdinalIgnoreCase)) { $rel = $rel.Substring($rootListed.Length).TrimStart('\', '/') }
    $rows += , @($p.id, @{ text = $rel; cls = 'mono' }, (Format-DiagBytes $f.bytes), [string]$f.records, [string]$f.bad, $(if ($f.truncated) { 'yes' } else { '' }))
  }
}
Add-Html (New-DiagHtmlTable -Headers @('Process', 'File', 'Size', 'Records', 'Bad lines', 'Truncated end') -Rows $rows -NumericColumns @(2, 3, 4))
if ($crashFiles.Count -gt 0) {
  Add-Html '<h3>Crash reports</h3>'
  $rows = @()
  foreach ($f in $crashFiles) { $rows += , @(@{ text = $f.FullName; cls = 'mono' }, (Format-DiagBytes $f.Length), $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss', $Inv)) }
  Add-Html (New-DiagHtmlTable -Headers @('File', 'Size', 'Written') -Rows $rows -NumericColumns @(1))
  foreach ($f in ($crashFiles | Where-Object { $_.Extension -eq '.txt' } | Select-Object -First 5)) {
    try {
      $txt = [System.IO.File]::ReadAllText($f.FullName)
      if ($txt.Length -gt 6000) { $txt = $txt.Substring(0, 6000) + "`n..." }
      Add-Html ('<details><summary>' + (Esc $f.Name) + '</summary><pre>' + (Esc $txt) + '</pre></details>')
    } catch { }
  }
}
foreach ($f in ($logFiles | Where-Object { $_.Name -eq 'scantailor.log' } | Select-Object -First 10)) {
  try {
    $lines = @(Get-Content -LiteralPath $f.FullName -Tail 40 -ErrorAction Stop)
    $rel = $f.FullName
    if ($rel.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) { $rel = $rel.Substring($root.Length).TrimStart('\', '/') } elseif ($rel.StartsWith($rootListed, [System.StringComparison]::OrdinalIgnoreCase)) { $rel = $rel.Substring($rootListed.Length).TrimStart('\', '/') }
    Add-Html ('<details><summary>Last lines of ' + (Esc $rel) + '</summary><pre>' + (Esc ($lines -join "`n")) + '</pre></details>')
  } catch { }
}
Add-Html ('<p class="muted">Analysed with Windows PowerShell ' + $PSVersionTable.PSVersion.ToString() + ' in ' + (F1 $swAll.Elapsed.TotalSeconds) + ' s. Format: SCHEMA.md.</p>')

# Assemble
$title = 'Scantailor-DGI diagnostics'
if ($label) { $title += ' - ' + $label }
$sub = @()
if ($summary.computer) { $sub += $summary.computer }
$sub += $(if ($isStress) { 'stress test' } else { 'everyday logs' })
$sub += ([string]$procs.Count + ' process(es)')
$spanStart = $null
foreach ($p in $procs) { if ($p.wall_start -and ($null -eq $spanStart -or $p.wall_start -lt $spanStart)) { $spanStart = $p.wall_start } }
if ($spanStart) { $sub += ('from ' + $spanStart.ToString('yyyy-MM-dd HH:mm', $Inv)) }
$sub += ('generated ' + (Get-Date).ToString('yyyy-MM-dd HH:mm', $Inv))
$doc = New-Object System.Text.StringBuilder
[void]$doc.Append('<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">')
[void]$doc.Append('<title>' + (Esc $title) + '</title><style>' + (Get-DiagReportCss) + '</style></head><body><main>')
[void]$doc.Append('<h1>' + (Esc $title) + '</h1><div class="sub">' + (Esc ($sub -join ' | ')) + '</div>')
[void]$doc.Append('<nav class="toc">' + ($toc -join '') + '</nav>')
[void]$doc.Append($HtmlBody.ToString())
[void]$doc.Append('</main></body></html>')

$reportPath = Join-Path $Out 'report.html'
Write-DiagText -Path $reportPath -Text $doc.ToString()
Write-DiagJson -Path (Join-Path $Out 'summary.json') -InputObject $summary -Depth 10

# CSV side tables (invariant culture, comma-separated)
try {
  $csv = New-Object System.Text.StringBuilder
  [void]$csv.AppendLine('process,when,phase,attributed_to,dur_ms,ongoing,ops,ops_last,stack_top,unended')
  foreach ($x in $allStalls) {
    $s = $x.s
    $fields = @($x.proc.id, (Get-StallWhen $x.proc $s), [string]$s.phase, $StallClassText[$x.cls], ([double]$x.dur).ToString('0.###', $Inv), [string][bool]$s.ongoing, (@($s.ops) -join ' > '), (@($s.ops_last) -join ' > '), (@($s.stack | Select-Object -First 8) -join ' | '), [string][bool]$s.unended)
    [void]$csv.AppendLine((($fields | ForEach-Object { '"' + ([string]$_).Replace('"', '""') + '"' }) -join ','))
  }
  Write-DiagText -Path (Join-Path $Out 'stalls.csv') -Text $csv.ToString()
  $csv = New-Object System.Text.StringBuilder
  [void]$csv.AppendLine('name,thread,calls,total_ms,mean_ms,p50_ms,p95_ms,p99_ms,max_ms,sampled')
  foreach ($o in $opRows) {
    $vals = @($o.name, $o.th, [string]$o.n, (Format-DiagNumber $o.sum 3), (Format-DiagNumber $o.mean 3), (Format-DiagNumber $o.p50 3), (Format-DiagNumber $o.p95 3), (Format-DiagNumber $o.p99 3), (Format-DiagNumber $o.max 3), [string]$o.sampled)
    [void]$csv.AppendLine(($vals -join ','))
  }
  Write-DiagText -Path (Join-Path $Out 'ops.csv') -Text $csv.ToString()
} catch {
  Write-DiagLog ('CSV export failed: ' + $_.Exception.Message) 'warn'
}

# summary.md: the verdicts and key numbers in Markdown, for a CI job summary
# or a message. Plain ASCII apart from what the logs themselves contain.
try {
  function ConvertTo-MdCell([string]$s) { return ($s -replace '\|', '/' -replace '[\r\n]+', ' ') }
  $md = New-Object System.Text.StringBuilder
  $mdTitle = 'Scantailor-DGI diagnostics'
  if ($label) { $mdTitle += ': ' + $label }
  [void]$md.AppendLine('# ' + (ConvertTo-MdCell $mdTitle))
  [void]$md.AppendLine('')
  [void]$md.AppendLine('**Overall: ' + $overall + '**' + $(if ($hardFail) { ' (hard failure)' } else { '' }) + ' - ' + (ConvertTo-MdCell ($sub -join ' | ')))
  [void]$md.AppendLine('')
  if ($partialNote) {
    [void]$md.AppendLine('> **' + (ConvertTo-MdCell $partialNote) + '**')
    [void]$md.AppendLine('')
  }
  [void]$md.AppendLine('| Status | Check | Numbers |')
  [void]$md.AppendLine('|---|---|---|')
  foreach ($c in $checks) { [void]$md.AppendLine('| ' + $c.status + ' | ' + (ConvertTo-MdCell $c.name) + ' | ' + (ConvertTo-MdCell $c.summary) + ' |') }
  if ($procs.Count -gt 0) {
    [void]$md.AppendLine('')
    [void]$md.AppendLine('| Process | Exit | Duration | Cycles | Stalls | Longest | Private MB/cycle | Page load p95 |')
    [void]$md.AppendLine('|---|---|---|---|---|---|---|---|')
    foreach ($x in $instSummaries) {
      $mem = $x.leak | Where-Object { $_.metric -eq 'private_mb' } | Select-Object -First 1
      [void]$md.AppendLine('| ' + (ConvertTo-MdCell $x.id) + ' | ' + (Format-ExitCode $x.exit_code) + ' | ' + (FMs ($x.duration_s * 1000)) + ' | ' + $x.cycles_done + ' | ' + $x.stalls.count + ' | ' + (FMs $x.stalls.longest_ms) + ' | ' + $(if ($mem -and $null -ne $mem.slope) { Format-DiagNumber $mem.slope 2 } else { '-' }) + ' | ' + (FMs $x.page_load.p95) + ' |')
    }
  }
  $topStalls = @($allStalls | Where-Object { $_.cls -eq 'run' } | Select-Object -First 5)
  if ($topStalls.Count -gt 0) {
    [void]$md.AppendLine('')
    [void]$md.AppendLine('Longest stalls: ' + (($topStalls | ForEach-Object {
            $in = @($_.s.ops_last | Where-Object { $_ })
            if ($in.Count -eq 0) { $in = @($_.s.ops | Where-Object { $_ }) }
            $w = '(none)'
            if ($in.Count -gt 0) { $w = [string]$in[$in.Count - 1] }
            $(if ($_.s.unended) { '>= ' } else { '' }) + (FMs $_.dur) + ' in ' + (ConvertTo-MdCell $w) + ' (' + $_.proc.id + $(if ($_.s.unended) { ', log ends during it' } else { '' }) + ')'
          }) -join '; ') + '.')
  }
  [void]$md.AppendLine('')
  [void]$md.AppendLine('Full report: report.html. Timing and memory verdicts are informational for the exit code.')
  Write-DiagText -Path (Join-Path $Out 'summary.md') -Text $md.ToString()
} catch {
  Write-DiagLog ('summary.md failed: ' + $_.Exception.Message) 'warn'
}

if ($tempExtract) {
  try { Remove-Item -LiteralPath $tempExtract -Recurse -Force -ErrorAction Stop } catch { }
}

if (-not $Quiet) {
  Write-Host ''
  if ($partialNote) { Write-Host $partialNote -ForegroundColor Yellow }
  Write-Host ('Overall: ' + $overall) -ForegroundColor $(if ($overall -eq 'FAIL') { 'Red' } elseif ($overall -eq 'WARN') { 'Yellow' } else { 'Green' })
  foreach ($c in $checks) {
    $col = 'Gray'
    if ($c.status -eq 'FAIL') { $col = 'Red' } elseif ($c.status -eq 'WARN') { $col = 'Yellow' } elseif ($c.status -eq 'PASS') { $col = 'Green' }
    Write-Host ('  ' + $c.status.PadRight(5) + ' ' + $c.name) -ForegroundColor $col
  }
  Write-Host ''
  Write-Host ('Report:  ' + $reportPath)
  Write-Host ('Summary: ' + (Join-Path $Out 'summary.json'))
}
if ($Open) {
  try { Start-Process -FilePath $reportPath } catch { }
}
if ($perfFiles.Count -eq 0 -or $hardFail) { exit 1 }
exit 0
