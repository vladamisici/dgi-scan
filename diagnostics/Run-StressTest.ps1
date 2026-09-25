<#
.SYNOPSIS
  Runs Scantailor-DGI's built-in stress scenario, measures the PC around it and
  writes a report.

.DESCRIPTION
  1. Takes an inventory of the PC and benchmarks the storage involved.
  2. Starts one or more instances of scantailor.exe --stress <config.json>,
     each in its own work folder, all at the same time.
  3. Samples every instance from outside every 5 s (runner-samples.csv).
  4. Waits for them (or kills them after -TimeoutMinutes, with a screenshot).
  5. Removes the application-data folders the stress instances left behind.
  6. Runs Analyze-Diagnostics.ps1 and prints where report.html is.

  Nothing is written outside -OutDir except the application's own stress
  folders under %LOCALAPPDATA%\scantailor-dgi, which are removed at the end.
  The operator's settings and recent-projects list are not touched: with
  -UseOperatorSettings a copy of the settings file is used.

.EXAMPLE
  .\Run-StressTest.ps1 -Quick
  A short smoke test on generated pages.

.EXAMPLE
  .\Run-StressTest.ps1 -Scans D:\Titluri\T123\scans -Pages 40 -Cycles 5 -CopyScansLocal -ProbeDir \\nas\titluri -Label "PC Milena"

.EXAMPLE
  .\Run-StressTest.ps1 -Synthetic 40 -Instances 3 -UseOperatorSettings -TimeoutMinutes 120

.NOTES
  Exit code: 0 = every instance exited 0 and there is no hard failure (crash,
  task failure, unexpected dialog, failed page load); 1 = otherwise (an
  instance that exited non-zero or hit the timeout always gives 1, even when
  the analysis failed); 2 = the test could not be run as asked (bad
  parameters, an -OutDir that already holds a run, an instance that could
  not be started, a runner error, or a failed analysis with no instance
  failure). Timing and memory verdicts are in the report only.
  OutDir\summary.md holds the verdicts in Markdown (used as a CI job summary).

  -OutDir must be a new or empty folder (or one without an earlier run):
  logs, crash files and settings of two runs must not mix. Without -OutDir a
  new timestamped folder is used.
#>
[CmdletBinding()]
param(
  [string]$Exe,
  [string]$Scans,
  [int]$Synthetic = 0,
  [ValidateSet('gray', 'rgb', 'bw')][string]$SyntheticKind = 'gray',
  [int]$SyntheticDpi = 300,
  [int]$SpreadEvery = 0,
  [int]$Pages = 40,
  [ValidateRange(1, 16)][int]$Instances = 1,
  [ValidateRange(1, 1000)][int]$Cycles = 5,
  [ValidateSet('reopen', 'fresh')][string]$Mode = 'reopen',
  [int]$NavPages = 10,
  [string[]]$Stages = @('0', '1', '2', '3', '4', '5'),
  [int]$AutosavesPerCycle = 2,
  [switch]$NoThumbScroll,
  [int]$SettleMs = 3000,
  [int]$BatchTimeoutSecPerPage = 180,
  [int]$PageLoadTimeoutSec = 180,
  [string]$OutDir,
  [string[]]$ProbeDir = @(),
  [switch]$UseOperatorSettings,
  [object]$Settings,
  [int]$TimeoutMinutes = 60,
  [switch]$CopyScansLocal,
  [switch]$SkipStorageProbe,
  [switch]$QuickProbe,
  [string]$Label = '',
  [switch]$Quick,
  [int]$StaggerSeconds = 0,
  [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
try {
  Get-ChildItem -LiteralPath $PSScriptRoot -File -ErrorAction Stop |
    Where-Object { $_.Extension -match '^\.(ps1|psm1|cmd)$' } | Unblock-File -ErrorAction SilentlyContinue
} catch { }
Import-Module (Join-Path $PSScriptRoot 'DiagnosticsCommon.psm1') -DisableNameChecking
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

function Split-ListArg([string[]]$Values) {
  # "powershell -File" passes "a,b" as one string, so lists are split here.
  # Paths are split on ';' always, and on ',' only when every piece is a path.
  $out = @()
  foreach ($v in @($Values)) {
    if (-not $v) { continue }
    foreach ($part in ($v -split ';')) {
      $part = $part.Trim()
      if (-not $part) { continue }
      $pieces = @($part -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
      $allPaths = $pieces.Count -gt 1
      foreach ($pc in $pieces) { if ($pc -notmatch '^(\\\\|[A-Za-z]:)') { $allPaths = $false } }
      if ($allPaths) { $out += $pieces } else { $out += $part }
    }
  }
  return , $out
}

function ConvertTo-SettingsMap($value) {
  # -Settings @{ key = value } from PowerShell, or "key=value;key2=value2"
  # from a .cmd, for keys under [settings] in the application's INI.
  $map = [ordered]@{}
  if ($null -eq $value) { return $map }
  if ($value -is [System.Collections.IDictionary]) {
    foreach ($k in $value.Keys) { $map[[string]$k] = $value[$k] }
    return $map
  }
  foreach ($pair in ([string]$value -split ';')) {
    if (-not $pair.Trim()) { continue }
    $kv = $pair -split '=', 2
    $k = $kv[0].Trim()
    $v = ''
    if ($kv.Count -gt 1) { $v = $kv[1].Trim() }
    $d = 0.0
    if ($v -eq 'true') { $map[$k] = $true }
    elseif ($v -eq 'false') { $map[$k] = $false }
    elseif ($v -match '^-?\d+$') { $map[$k] = [long]$v }
    elseif ([double]::TryParse($v, [System.Globalization.NumberStyles]::Float, $Inv, [ref]$d)) { $map[$k] = $d }
    else { $map[$k] = $v }
  }
  return $map
}

function Find-Exe {
  $cands = @(
    (Join-Path $PSScriptRoot '..\scantailor.exe'),
    (Join-Path $PSScriptRoot 'scantailor.exe'),
    (Join-Path $PSScriptRoot '..\build\Release\scantailor.exe'),
    (Join-Path $PSScriptRoot '..\build\RelWithDebInfo\scantailor.exe')
  )
  if ($env:ProgramFiles) { $cands += (Join-Path $env:ProgramFiles 'Scantailor-DGI\scantailor.exe') }
  if (${env:ProgramFiles(x86)}) { $cands += (Join-Path ${env:ProgramFiles(x86)} 'Scantailor-DGI\scantailor.exe') }
  foreach ($c in $cands) {
    if (Test-Path -LiteralPath $c -PathType Leaf) { return (Resolve-Path -LiteralPath $c).ProviderPath }
  }
  return $null
}

function Save-Screenshot([string]$Path) {
  # Taken before a hung instance is killed: whatever dialog or state it was
  # stuck in is usually the most useful single piece of evidence.
  try {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing -ErrorAction Stop
    if (Initialize-DiagNative) { try { [void][StDiag.Native]::SetProcessDPIAware() } catch { } }
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
      $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
      $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
      $g.Dispose(); $bmp.Dispose()
    }
    return $true
  } catch {
    Write-DiagLog ('Screenshot failed (locked or disconnected session?): ' + $_.Exception.Message) 'warn'
    return $false
  }
}

function Get-ExitMeaning($code) {
  if ($null -eq $code) { return 'unknown' }
  switch ([long]$code) {
    0 { return 'finished, no failures' }
    2 { return 'finished with failures' }
    3 { return 'fatal (configuration, out of memory or project creation)' }
    -1 { return 'killed' }
  }
  $hex = '0x' + ([int][long]$code).ToString('X8')
  $known = @{ '0xC0000005' = 'access violation'; '0xC0000409' = 'fail-fast / stack buffer overrun'; '0xC00000FD' = 'stack overflow'; '0xC0000374' = 'heap corruption'; '0xC0000017' = 'out of memory'; '0x40010004' = 'terminated by debugger/console'; '0xC000013A' = 'terminated (Ctrl+C / logoff)' }
  $m = $known[$hex]
  if ($m) { return ('crash: ' + $m + ' (' + $hex + ')') }
  return ('crash (' + $hex + ')')
}

function Update-LogTail($st) {
  # Reads what the instance appended to its log since last time, for the
  # progress line. The file is opened shared; the application keeps writing.
  try {
    $logs = Join-Path $st.dir 'logs'
    if (-not (Test-Path -LiteralPath $logs)) { return }
    $f = Get-ChildItem -LiteralPath $logs -Filter 'scantailor-perf-*.jsonl' -File -ErrorAction SilentlyContinue | Sort-Object CreationTimeUtc, Name | Select-Object -Last 1
    if (-not $f) { return }
    if ($st.tail_file -ne $f.FullName) { $st.tail_file = $f.FullName; $st.tail_pos = [long]0 }
    $fs = [System.IO.FileStream]::new($f.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete')
    try {
      # The size in the directory entry lags behind while the application
      # still has the file open; the open stream's length is current.
      $size = $fs.Length
      if ($size -le $st.tail_pos) { return }
      [void]$fs.Seek($st.tail_pos, [System.IO.SeekOrigin]::Begin)
      $len = [int][Math]::Min(16MB, $size - $st.tail_pos)
      $buf = New-Object byte[] $len
      $n = $fs.Read($buf, 0, $len)
      $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
      $cut = $text.LastIndexOf([char]10)
      if ($cut -lt 0) { return }
      $st.tail_pos += [System.Text.Encoding]::UTF8.GetByteCount($text.Substring(0, $cut + 1))
      $text = $text.Substring(0, $cut + 1)
      foreach ($m in [regex]::Matches($text, '"ev":"stress\.cycle_begin".*?"cycle":(\d+)')) { $st.cycle = [int]$m.Groups[1].Value }
      foreach ($m in [regex]::Matches($text, '"ev":"stall".*?"dur":([\d.]+)')) {
        $st.stalls++
        $d = [double]::Parse($m.Groups[1].Value, $Inv)
        if ($d -gt $st.longest) { $st.longest = $d }
      }
      # A stall still going: the watchdog writes stall_progress records while
      # it lasts and a stall record with the same "at" when it ends.
      foreach ($m in [regex]::Matches($text, '(?m)^.*"ev":"stall(?:_progress)?".*$')) {
        $line = $m.Value
        $mAt = [regex]::Match($line, '"at":([\d.]+)')
        if (-not $mAt.Success) { continue }
        $at = [double]::Parse($mAt.Groups[1].Value, $Inv)
        if ($line.Contains('"ev":"stall_progress"')) {
          $mDur = [regex]::Match($line, '"dur":([\d.]+)')
          $d = 0.0
          if ($mDur.Success) { $d = [double]::Parse($mDur.Groups[1].Value, $Inv) }
          $st.open_stall = @{ at = $at; dur = $d }
        } elseif ($st.open_stall -and [Math]::Abs($st.open_stall.at - $at) -lt 0.01) {
          $st.open_stall = $null
        }
      }
      $st.failures += ([regex]::Matches($text, '"ev":"stress\.(failure|unexpected_dialog)"')).Count
      if ($text -match '"ev":"stress\.finished"') { $st.finished = $true }
    } finally { $fs.Dispose() }
  } catch { }
}

# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------

if ($Quick) {
  if (-not $PSBoundParameters.ContainsKey('Pages')) { $Pages = 12 }
  if (-not $PSBoundParameters.ContainsKey('Cycles')) { $Cycles = 2 }
  if (-not $PSBoundParameters.ContainsKey('NavPages')) { $NavPages = 4 }
  if (-not $PSBoundParameters.ContainsKey('TimeoutMinutes')) { $TimeoutMinutes = 20 }
  $QuickProbe = $true
}
$ProbeDir = Split-ListArg $ProbeDir
# Relative paths are relative to PowerShell's current location; the .NET
# calls that use them would resolve them against the process directory.
$ProbeDir = @($ProbeDir | ForEach-Object {
    $pd = $_
    try { $pd = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($pd) } catch { }
    $pd
  })
$stageList = @()
foreach ($s in (Split-ListArg $Stages)) { foreach ($x in ($s -split '[,\s]+')) { if ($x -match '^\d+$') { $stageList += [int]$x } } }
if ($stageList.Count -eq 0) { $stageList = @(0, 1, 2, 3, 4, 5) }
$settingsMap = ConvertTo-SettingsMap $Settings

if (-not $Exe) { $Exe = Find-Exe }
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
  Write-DiagLog 'scantailor.exe not found. Pass -Exe "C:\path\to\scantailor.exe".' 'error'
  exit 2
}
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath
$exeVersion = ''
try { $vi = (Get-Item -LiteralPath $Exe).VersionInfo; $exeVersion = ([string]$vi.ProductVersion).Trim(); if (-not $exeVersion) { $exeVersion = [string]$vi.FileVersion } } catch { }

if ($Scans -and $Synthetic -gt 0) {
  Write-DiagLog 'Use either -Scans or -Synthetic, not both.' 'error'
  exit 2
}
if (-not $Scans -and $Synthetic -le 0) {
  $Synthetic = [Math]::Max(1, $Pages)
  Write-DiagLog ('No -Scans given: using ' + $Synthetic + ' generated pages.')
}
$scanFiles = @()
if ($Scans) {
  if (-not (Test-Path -LiteralPath $Scans -PathType Container)) {
    Write-DiagLog ('Scans folder not found: ' + $Scans) 'error'
    exit 2
  }
  $Scans = (Resolve-Path -LiteralPath $Scans).ProviderPath
  $scanFiles = @(Get-ChildItem -LiteralPath $Scans -File | Where-Object { $_.Extension -match '^\.(tif|tiff|jpg|jpeg|png)$' } | Sort-Object { Get-DiagNaturalKey $_.Name })
  if ($scanFiles.Count -eq 0) {
    Write-DiagLog ('No .tif/.jpg/.png images in ' + $Scans) 'error'
    exit 2
  }
  if (-not (Test-DiagAsciiPath $Scans)) { Write-DiagLog 'The scans path has non-ASCII characters; the application may not handle it. A path like D:\stress\scans is safer.' 'warn' }
}

$defaultOut = $false
if (-not $OutDir) {
  $base = $env:LOCALAPPDATA
  if (-not $base) { $base = Get-DiagTempRoot }
  $OutDir = Join-Path $base ('scantailor-dgi-stress\' + (Get-Date).ToString('yyyyMMdd-HHmmss', $Inv))
  $defaultOut = $true
}
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
if (Test-Path -LiteralPath $OutDir -PathType Leaf) {
  Write-DiagLog ('-OutDir is a file, not a folder: ' + $OutDir) 'error'
  exit 2
}
if ($defaultOut) {
  # Two runs started in the same second: never share a folder.
  $n = 2
  $first = $OutDir
  while (Test-Path -LiteralPath $OutDir) { $OutDir = $first + '-' + $n; $n++ }
} elseif (Test-Path -LiteralPath $OutDir -PathType Container) {
  # An earlier run's logs, crash files and settings INI would be analysed
  # together with this run's (and its settings reused), so a folder that
  # already holds a run is refused instead of silently mixed.
  $prev = @(Get-ChildItem -LiteralPath $OutDir -Force -ErrorAction SilentlyContinue | Where-Object { ($_.Name -eq 'run.json' -and -not $_.PSIsContainer) -or ($_.PSIsContainer -and $_.Name -match '^inst\d+$') })
  if ($prev.Count -gt 0) {
    Write-DiagLog ('-OutDir already holds a stress run (' + (($prev | Select-Object -First 4 | ForEach-Object { $_.Name }) -join ', ') + '): ' + $OutDir) 'error'
    Write-DiagLog 'Its logs, crash reports and settings would mix with this run. Pass a new (or empty) folder with -OutDir, or leave -OutDir out for a new timestamped folder.' 'error'
    exit 2
  }
}
[void][System.IO.Directory]::CreateDirectory($OutDir)
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath
if ($Scans) {
  $sn = $Scans.TrimEnd('\') + '\'
  if (($OutDir.TrimEnd('\') + '\').StartsWith($sn, [System.StringComparison]::OrdinalIgnoreCase)) {
    Write-DiagLog 'OutDir must not be inside the scans folder: the test writes projects and outputs there.' 'error'
    exit 2
  }
}
if (-not (Test-DiagAsciiPath $OutDir)) { Write-DiagLog 'OutDir has non-ASCII characters; the application may not handle it. Pass -OutDir C:\stress or similar.' 'warn' }

$transcript = $false
try { Start-Transcript -LiteralPath (Join-Path $OutDir 'runner.log') -Append | Out-Null; $transcript = $true } catch { }

$runStarted = Get-Date
$params = [ordered]@{
  Exe = $Exe; Scans = $Scans; Synthetic = $Synthetic; SyntheticKind = $SyntheticKind; SyntheticDpi = $SyntheticDpi; SpreadEvery = $SpreadEvery
  Pages = $Pages; Instances = $Instances; Cycles = $Cycles; Mode = $Mode; NavPages = $NavPages; Stages = $stageList
  AutosavesPerCycle = $AutosavesPerCycle; ThumbScroll = (-not $NoThumbScroll); SettleMs = $SettleMs
  BatchTimeoutSecPerPage = $BatchTimeoutSecPerPage; PageLoadTimeoutSec = $PageLoadTimeoutSec; OutDir = $OutDir; ProbeDir = $ProbeDir
  UseOperatorSettings = [bool]$UseOperatorSettings; Settings = $settingsMap; TimeoutMinutes = $TimeoutMinutes
  CopyScansLocal = [bool]$CopyScansLocal; SkipStorageProbe = [bool]$SkipStorageProbe; QuickProbe = [bool]$QuickProbe; Quick = [bool]$Quick
  StaggerSeconds = $StaggerSeconds; Label = $Label
}
$run = [ordered]@{
  tool = 'Run-StressTest ' + (Get-DiagSuiteVersion)
  label = $Label
  computer = Get-DiagComputerName
  started = $runStarted.ToString('yyyy-MM-ddTHH:mm:sszzz', $Inv)
  ended = $null
  exe = $Exe
  exe_version = $exeVersion
  params = $params
  instances = @()
  inventory = $null
  storage = @()
  copy_scans = $null
  operator_settings = $null
  notes = @()
  aborted = $false
}
$runJson = Join-Path $OutDir 'run.json'
$procs = @()
$states = @()
$exitCode = 0

Write-DiagLog ('Scantailor-DGI stress test -> ' + $OutDir)
Write-DiagLog ('Executable: ' + $Exe + $(if ($exeVersion) { ' (' + $exeVersion + ')' } else { '' }))

try {
  $others = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'scantailor*' })
  if ($others.Count -gt 0) {
    $msg = [string]$others.Count + ' other Scantailor process(es) already running; they share the CPU and disk with the test.'
    Write-DiagLog $msg 'warn'
    $run.notes += $msg
  }

  # 1. Inventory
  Write-DiagLog 'Collecting PC inventory ...'
  $paths = @($OutDir)
  if ($Scans) { $paths += $Scans }
  $paths += $ProbeDir
  $run.inventory = Get-SystemInventory -Paths $paths
  Write-DiagJson -Path $runJson -InputObject $run

  # 2. Storage
  if (-not $SkipStorageProbe) {
    $probes = @(@{ Path = $OutDir; Label = 'Work folder (OutDir)'; Role = 'workdir'; ReadOnly = $false })
    if ($Scans) { $probes += @{ Path = $Scans; Label = 'Scans folder (read only)'; Role = 'scans'; ReadOnly = $true } }
    $pi = 0
    foreach ($pd in $ProbeDir) { $pi++; $probes += @{ Path = $pd; Label = ('Probe ' + $pi + ': ' + $pd); Role = 'probe'; ReadOnly = $false } }
    foreach ($pr in $probes) {
      Write-DiagLog ('Storage benchmark: ' + $pr.Label + ' ...')
      $r = Measure-Storage -Path $pr.Path -Label $pr.Label -Role $pr.Role -Quick:$QuickProbe -ReadOnly:$pr.ReadOnly
      $run.storage += $r
      $t = $r.tests
      $line = '  ' + $r.mode
      if ($t.durable_1mb) { $line += ', durable 1 MB p50 ' + (Format-DiagMs $t.durable_1mb.p50) }
      if ($t.small_create) { $line += ', create p95 ' + (Format-DiagMs $t.small_create.p95) }
      if ($t.stat) { $line += ', stat p50 ' + (Format-DiagMs $t.stat.p50) }
      if ($t.seq_write) { $line += ', write ' + (Format-DiagNumber $t.seq_write.mb_per_s 0) + ' MB/s' }
      if ($t.seq_read) { $line += ', read ' + (Format-DiagNumber $t.seq_read.mb_per_s 0) + ' MB/s' }
      Write-DiagLog $line
      foreach ($h in (Get-StorageHints $r)) { Write-DiagLog ('  ' + $h) 'warn' }
      foreach ($e in @($r.errors)) { if ($e) { Write-DiagLog ('  ' + $e) 'warn' } }
      foreach ($nt in @($r.notes)) { if ($nt) { Write-DiagLog ('  ' + $nt) } }
    }
    Write-DiagJson -Path $runJson -InputObject $run
  }

  # 3. Scans
  $scansDir = ''
  $maxPages = $Pages
  if ($Scans) {
    $scansDir = $Scans
    if ($CopyScansLocal) {
      $dest = Join-Path $OutDir 'scans'
      [void][System.IO.Directory]::CreateDirectory($dest)
      $subset = $scanFiles
      if ($Pages -gt 0) { $subset = @($scanFiles | Select-Object -First $Pages) }
      Write-DiagLog ('Copying ' + $subset.Count + ' scans to ' + $dest + ' ...')
      $sw = [System.Diagnostics.Stopwatch]::StartNew()
      $bytes = [long]0
      foreach ($f in $subset) {
        Copy-DiagSharedFile -Source $f.FullName -Destination (Join-Path $dest $f.Name)
        $bytes += $f.Length
      }
      $sec = $sw.Elapsed.TotalSeconds
      $run.copy_scans = [ordered]@{ files = $subset.Count; mb = [Math]::Round($bytes / 1MB, 1); seconds = [Math]::Round($sec, 1); mb_per_s = [Math]::Round(($bytes / 1MB) / [Math]::Max($sec, 0.001), 1); from = $Scans }
      Write-DiagLog ('  ' + (Format-DiagBytes $bytes) + ' in ' + (Format-DiagNumber $sec 1) + ' s (' + (Format-DiagNumber $run.copy_scans.mb_per_s 1) + ' MB/s)')
      $scansDir = $dest
      $maxPages = 0
    }
  }

  # 4. Operator settings (a copy, so the operator's file cannot be changed)
  $opIniCopy = ''
  if ($UseOperatorSettings) {
    $cands = @()
    $cands += (Join-Path (Split-Path -Parent $Exe) 'config\scantailor-dgi\scantailor-dgi.ini')
    if ($env:APPDATA) { $cands += (Join-Path $env:APPDATA 'scantailor-dgi\scantailor-dgi.ini') }
    $src = $null
    foreach ($c in $cands) { if (Test-Path -LiteralPath $c -PathType Leaf) { $src = $c; break } }
    if ($src) {
      $opIniCopy = Join-Path $OutDir 'operator-settings.ini'
      Copy-DiagSharedFile -Source $src -Destination $opIniCopy
      $run.operator_settings = $src
      Write-DiagLog ('Using a copy of the operator settings: ' + $src)
    } else {
      Write-DiagLog ('-UseOperatorSettings: no settings file found (' + ($cands -join '; ') + '); using defaults.') 'warn'
      $run.notes += 'Operator settings requested but not found.'
    }
  }

  # 5. Configs, and stale application folders from an earlier interrupted run
  $appRoot = $null
  if ($env:LOCALAPPDATA) { $appRoot = Join-Path $env:LOCALAPPDATA 'scantailor-dgi' }
  for ($k = 1; $k -le $Instances; $k++) {
    $dir = Join-Path $OutDir ('inst' + $k)
    [void][System.IO.Directory]::CreateDirectory($dir)
    $cfg = [ordered]@{
      instance = $k
      workDir = $dir
      scansDir = $scansDir
      maxPages = $maxPages
      synthetic = [ordered]@{ count = $(if ($Scans) { 0 } else { $Synthetic }); kind = $SyntheticKind; dpi = $SyntheticDpi; spreadEvery = $SpreadEvery }
      cycles = $Cycles
      mode = $Mode
      navPages = $NavPages
      stages = $stageList
      autosavesPerCycle = $AutosavesPerCycle
      thumbScroll = (-not $NoThumbScroll)
      settleMs = $SettleMs
      batchTimeoutSecPerPage = $BatchTimeoutSecPerPage
      pageLoadTimeoutSec = $PageLoadTimeoutSec
      operatorSettingsFile = $opIniCopy
      settings = $settingsMap
    }
    $cfgPath = Join-Path $dir 'stress-config.json'
    Write-DiagJson -Path $cfgPath -InputObject $cfg
    if ($appRoot) {
      $stale = Join-Path $appRoot ('scantailor-dgi-stress-' + $k)
      if (Test-Path -LiteralPath $stale) {
        try { Remove-Item -LiteralPath $stale -Recurse -Force -ErrorAction Stop; Write-DiagLog ('Removed leftover ' + $stale) } catch { Write-DiagLog ('Could not remove ' + $stale + ': ' + $_.Exception.Message) 'warn' }
      }
    }
    $states += @{ k = $k; dir = $dir; cfg = $cfgPath; proc = $null; started = $null; ended = $null; exit = $null; timed_out = $false; killed = $false; screenshot = $null; title = $null
      cpu_prev = $null; t_prev = $null; tail_file = $null; tail_pos = [long]0; cycle = 0; stalls = 0; longest = 0.0; failures = 0; finished = $false; last_private = $null; last_cpu = $null; start_error = $null; open_stall = $null }
  }
  Write-DiagJson -Path $runJson -InputObject $run

  # 6. Launch
  $cores = [Environment]::ProcessorCount
  $native = Initialize-DiagNative
  foreach ($st in $states) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = '--stress "' + $st.cfg + '"'
    $psi.WorkingDirectory = $st.dir
    $psi.UseShellExecute = $false
    # Stress mode always logs verbosely; an inherited SCANTAILOR_DIAG=off must not silence it.
    if ($psi.EnvironmentVariables.ContainsKey('SCANTAILOR_DIAG')) { $psi.EnvironmentVariables.Remove('SCANTAILOR_DIAG') }
    try {
      $p = [System.Diagnostics.Process]::Start($psi)
      $st.proc = $p
      $st.started = Get-Date
      Write-DiagLog ('Started inst' + $st.k + ' (pid ' + $p.Id + ')')
    } catch {
      $st.start_error = $_.Exception.Message
      Write-DiagLog ('Could not start inst' + $st.k + ': ' + $_.Exception.Message) 'error'
    }
    if ($StaggerSeconds -gt 0) { Start-Sleep -Seconds $StaggerSeconds }
  }

  # 7. Monitor
  $csvPath = Join-Path $OutDir 'runner-samples.csv'
  $csv = New-Object System.IO.StreamWriter($csvPath, $false, (New-Object System.Text.UTF8Encoding($false)))
  $csv.WriteLine('time,elapsed_s,instance,pid,cpu_pct,private_mb,ws_mb,handles,threads,gdi,user,sys_avail_mb')
  $t0 = Get-Date
  $deadline = $t0.AddMinutes($TimeoutMinutes)
  $lastStatus = [datetime]::MinValue
  try {
    while ($true) {
      $now = Get-Date
      $running = @($states | Where-Object { $_.proc -and $null -eq $_.ended })
      if ($running.Count -eq 0) { break }
      $avail = ''
      try { $avail = [string][Math]::Round([double](Get-CimInstance -ClassName Win32_OperatingSystem -OperationTimeoutSec 5).FreePhysicalMemory / 1024) } catch { }
      foreach ($st in $running) {
        $p = $st.proc
        try { $p.Refresh() } catch { }
        if ($p.HasExited) {
          $p.WaitForExit()
          $st.ended = Get-Date
          $st.exit = $p.ExitCode
          Write-DiagLog ('inst' + $st.k + ' exited with ' + $st.exit + ' - ' + (Get-ExitMeaning $st.exit) + ' after ' + (Format-DiagMs (($st.ended - $st.started).TotalMilliseconds)))
          continue
        }
        try {
          $cpuNow = $p.TotalProcessorTime.TotalSeconds
          $cpuPct = ''
          if ($null -ne $st.cpu_prev) {
            $dt = ($now - $st.t_prev).TotalSeconds
            if ($dt -gt 0) { $cpuPct = (100.0 * ($cpuNow - $st.cpu_prev) / ($dt * $cores)).ToString('0.0', $Inv) }
          }
          $st.cpu_prev = $cpuNow; $st.t_prev = $now
          $gdi = ''; $user = ''
          if ($native) { try { $gdi = [string][StDiag.Native]::GetGuiResources($p.Handle, 0); $user = [string][StDiag.Native]::GetGuiResources($p.Handle, 1) } catch { } }
          $priv = $p.PrivateMemorySize64 / 1MB
          $st.last_private = $priv
          $st.last_cpu = $cpuPct
          $fields = @($now.ToString('yyyy-MM-ddTHH:mm:ss', $Inv), ($now - $t0).TotalSeconds.ToString('0', $Inv), [string]$st.k, [string]$p.Id, $cpuPct,
            $priv.ToString('0.0', $Inv), ($p.WorkingSet64 / 1MB).ToString('0.0', $Inv), [string]$p.HandleCount, [string]$p.Threads.Count, $gdi, $user, $avail)
          $csv.WriteLine(($fields -join ','))
        } catch { }
      }
      $csv.Flush()
      if (($now - $lastStatus).TotalSeconds -ge 30) {
        $lastStatus = $now
        $parts = @()
        foreach ($st in $states) {
          Update-LogTail $st
          if ($null -eq $st.proc) { $parts += ('inst' + $st.k + ': not started'); continue }
          if ($null -ne $st.ended) { $parts += ('inst' + $st.k + ': exit ' + $st.exit); continue }
          $txt = 'inst' + $st.k + ': cycle ' + $st.cycle + '/' + $Cycles + ', ' + (Format-DiagNumber $st.last_private 0) + ' MB'
          if ($st.stalls -gt 0) { $txt += ', ' + $st.stalls + ' stalls (max ' + (Format-DiagMs $st.longest) + ')' }
          if ($st.open_stall) { $txt += ', GUI STALLED for >= ' + (Format-DiagMs $st.open_stall.dur) }
          if ($st.failures -gt 0) { $txt += ', ' + $st.failures + ' failure(s)' }
          $parts += $txt
        }
        Write-DiagLog ('[' + (Format-DiagMs (($now - $t0).TotalMilliseconds)) + '] ' + ($parts -join ' | '))
      }
      if ($now -gt $deadline) {
        Write-DiagLog ('Overall timeout of ' + $TimeoutMinutes + ' min reached.') 'warn'
        $shotDone = $null
        foreach ($st in @($states | Where-Object { $_.proc -and $null -eq $_.ended })) {
          $shot = Join-Path $st.dir 'timeout-screenshot.png'
          if ($null -eq $shotDone) {
            if (Save-Screenshot $shot) { $shotDone = $shot } else { $shotDone = '' }
          } elseif ($shotDone) {
            Copy-Item -LiteralPath $shotDone -Destination $shot -Force
          }
          if ($shotDone) { $st.screenshot = $shot }
          try { $st.proc.Refresh(); $st.title = $st.proc.MainWindowTitle } catch { }
          Update-LogTail $st
          if ($st.open_stall) { Write-DiagLog ('inst' + $st.k + ': the GUI thread has been stalled for >= ' + (Format-DiagMs $st.open_stall.dur) + ' (last stall_progress record)') 'warn' }
          try { $st.proc.Kill(); $st.proc.WaitForExit(15000) | Out-Null } catch { }
          $st.timed_out = $true; $st.killed = $true
          $st.ended = Get-Date
          try { $st.exit = $st.proc.ExitCode } catch { $st.exit = $null }
          Write-DiagLog ('Killed inst' + $st.k + $(if ($st.title) { ' (window: "' + $st.title + '")' } else { '' })) 'warn'
        }
        break
      }
      Start-Sleep -Seconds 5
    }
  } finally {
    $csv.Dispose()
  }
  foreach ($st in $states) { Update-LogTail $st }
} catch {
  $exitCode = 2
  $run.notes += ('Runner error: ' + $_.Exception.Message)
  Write-DiagLog ('Runner error: ' + $_.Exception.Message) 'error'
} finally {
  # Also reached on Ctrl+C: instances left running would carry on unobserved.
  foreach ($st in $states) {
    if ($st.proc -and $null -eq $st.ended) {
      try { if (-not $st.proc.HasExited) { $st.proc.Kill(); $st.proc.WaitForExit(15000) | Out-Null; $st.killed = $true; $run.aborted = $true } } catch { }
      $st.ended = Get-Date
      try { $st.exit = $st.proc.ExitCode } catch { }
    }
  }

  # 8. Remove what the stress instances left in the user profile, keeping any
  #    crash reports or logs that ended up there.
  for ($k = 1; $k -le $Instances; $k++) {
    $name = 'scantailor-dgi-stress-' + $k
    $dirs = @()
    if ($env:LOCALAPPDATA) { $dirs += (Join-Path $env:LOCALAPPDATA ('scantailor-dgi\' + $name)) }
    foreach ($d in $dirs) {
      if (-not (Test-Path -LiteralPath $d)) { continue }
      try {
        $keep = @(Get-ChildItem -LiteralPath $d -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -match '^\.(dmp|txt|jsonl|log)$' })
        if ($keep.Count -gt 0) {
          $rescue = Join-Path $OutDir ('inst' + $k + '\logs\from-appdata')
          [void][System.IO.Directory]::CreateDirectory($rescue)
          foreach ($f in $keep) { Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $rescue $f.Name) -Force }
        }
        Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction Stop
      } catch { Write-DiagLog ('Could not remove ' + $d + ': ' + $_.Exception.Message) 'warn' }
    }
    $inis = @()
    if ($env:APPDATA) { $inis += (Join-Path $env:APPDATA ('scantailor-dgi\' + $name + '.ini')) }
    $inis += (Join-Path (Split-Path -Parent $Exe) ('config\scantailor-dgi\' + $name + '.ini'))
    foreach ($ini in $inis) {
      if (Test-Path -LiteralPath $ini -PathType Leaf) { try { Remove-Item -LiteralPath $ini -Force -ErrorAction Stop } catch { } }
    }
  }

  $run.ended = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz', $Inv)
  $run.instances = @()
  foreach ($st in $states) {
    $secs = $null
    if ($st.started -and $st.ended) { $secs = [Math]::Round(($st.ended - $st.started).TotalSeconds, 1) }
    $run.instances += [ordered]@{
      instance = $st.k; pid = $(if ($st.proc) { $st.proc.Id } else { $null }); dir = $st.dir; config = $st.cfg
      started = $(if ($st.started) { $st.started.ToString('yyyy-MM-ddTHH:mm:sszzz', $Inv) } else { $null })
      ended = $(if ($st.ended) { $st.ended.ToString('yyyy-MM-ddTHH:mm:sszzz', $Inv) } else { $null })
      seconds = $secs; exit_code = $st.exit; exit_meaning = (Get-ExitMeaning $st.exit)
      timed_out = $st.timed_out; killed = $st.killed; timeout_minutes = $TimeoutMinutes
      screenshot = $st.screenshot; window_title = $st.title; start_error = $st.start_error
    }
  }
  try { Write-DiagJson -Path $runJson -InputObject $run } catch { Write-DiagLog ('Could not write run.json: ' + $_.Exception.Message) 'error' }
}

# 9. Analyse
Write-Host ''
Write-DiagLog 'Analysing the logs ...'
$report = Join-Path $OutDir 'report\report.html'
try {
  & (Join-Path $PSScriptRoot 'Analyze-Diagnostics.ps1') -Path $OutDir
  $sum = Read-DiagJson -Path (Join-Path $OutDir 'report\summary.json')
  # Only hard failures set the exit code; timing and memory verdicts are in
  # the report but a busy or shared machine can trip them on its own.
  if ($sum.verdict.hard_fail -and $exitCode -eq 0) { $exitCode = 1 }
  $mdSrc = Join-Path $OutDir 'report\summary.md'
  if (Test-Path -LiteralPath $mdSrc) { Copy-Item -LiteralPath $mdSrc -Destination (Join-Path $OutDir 'summary.md') -Force }
} catch {
  Write-DiagLog ('Analysis failed: ' + $_.Exception.Message) 'error'
  $exitCode = 2
}
# An instance that ran and exited non-zero (or hit the timeout) is a test
# failure whatever else happened, a failed analysis included. Instances the
# runner itself killed after an error or Ctrl+C are not: their exit code is
# the kill's, not the application's.
foreach ($st in $states) {
  if ($null -eq $st.exit -or [long]$st.exit -eq 0) { continue }
  if ($st.killed -and -not $st.timed_out) { continue }
  $exitCode = 1
}
# An instance that could not be started means the test did not run as asked.
$notStarted = @($states | Where-Object { $null -eq $_.proc })
if ($notStarted.Count -gt 0) {
  foreach ($st in $notStarted) {
    $why = 'not started'
    if ($st.start_error) { $why = 'could not be started: ' + $st.start_error }
    Write-DiagLog ('inst' + $st.k + ' ' + $why) 'error'
  }
  Write-DiagLog ([string]$notStarted.Count + ' of ' + $states.Count + ' instance(s) did not run; exit code 2.') 'error'
  $exitCode = 2
}

Write-Host ''
Write-Host ('Output folder: ' + $OutDir)
if (Test-Path -LiteralPath $report) {
  Write-Host ('Report:        ' + $report) -ForegroundColor Cyan
  if (-not $NoOpen -and [Environment]::UserInteractive -and -not ($env:CI -or $env:GITHUB_ACTIONS -or $env:TF_BUILD)) { try { Start-Process -FilePath $report } catch { } }
}
if ($transcript) { try { Stop-Transcript | Out-Null } catch { } }
exit $exitCode
