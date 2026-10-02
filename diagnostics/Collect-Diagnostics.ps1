<#
.SYNOPSIS
  Collects Scantailor-DGI's everyday diagnostics logs from this PC into one
  folder and zip, with a PC inventory and a report.

.DESCRIPTION
  Looks in the places the application writes its logs:
    %LOCALAPPDATA%\scantailor-dgi\scantailor-dgi\crashes   (installed build)
    <application folder>\config\crashes                     (portable build)
    %TEMP%\scantailor-crashes                               (fallback when neither is writable)
  and copies, from the last -Days days: scantailor-perf-*.jsonl,
  scantailor.log*, crash notes (scantailor-*.txt) and, with -IncludeDumps,
  crash and stall minidumps (*.dmp, can be large). Files still open by a running
  Scantailor are copied as they are at that moment.

  Adds inventory.json (and storage.json when -ProbeDir is given), runs
  Analyze-Diagnostics.ps1, and zips everything as
  diag-<COMPUTERNAME>-<yyyyMMdd-HHmm>.zip on the Desktop (or in -OutDir).
  Nothing is deleted or changed in the log folders.

.EXAMPLE
  .\Collect-Diagnostics.ps1
.EXAMPLE
  .\Collect-Diagnostics.ps1 -AppDir "D:\Scantailor-DGI" -Days 3 -ProbeDir \\nas\titluri -Label "PC Milena"
#>
[CmdletBinding()]
param(
  [string]$AppDir,
  [int]$Days = 7,
  [switch]$IncludeDumps,
  [string[]]$ProbeDir = @(),
  [string]$OutDir,
  [string]$Label = '',
  [switch]$QuickProbe,
  [switch]$NoAnalyze,
  [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
try {
  Get-ChildItem -LiteralPath $PSScriptRoot -File -ErrorAction Stop |
    Where-Object { $_.Extension -match '^\.(ps1|psm1|cmd)$' } | Unblock-File -ErrorAction SilentlyContinue
} catch { }
Import-Module (Join-Path $PSScriptRoot 'DiagnosticsCommon.psm1') -DisableNameChecking
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

# Relative paths are relative to PowerShell's current location; the .NET
# calls that use them would resolve them against the process directory.
function ConvertTo-FullPath([string]$p) {
  try { return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p) } catch { return $p }
}
$probeList = @()
foreach ($v in @($ProbeDir)) { foreach ($p in ($v -split ';')) { if ($p.Trim()) { $probeList += (ConvertTo-FullPath $p.Trim()) } } }

if (-not $OutDir) {
  $OutDir = [Environment]::GetFolderPath('Desktop')
  if (-not $OutDir -or -not (Test-Path -LiteralPath $OutDir)) { $OutDir = Get-DiagTempRoot }
}
$OutDir = ConvertTo-FullPath $OutDir
[void][System.IO.Directory]::CreateDirectory($OutDir)
$computer = Get-DiagComputerName
$name = 'diag-' + $computer + '-' + (Get-Date).ToString('yyyyMMdd-HHmm', $Inv)
$dest = Join-Path $OutDir $name
[void][System.IO.Directory]::CreateDirectory($dest)
Write-DiagLog ('Collecting Scantailor-DGI diagnostics into ' + $dest)

# Where the logs can be. The application picks the portable location when its
# own folder is writable, so both are checked whatever the install type.
$sources = @()
if ($env:LOCALAPPDATA) { $sources += @{ tag = 'installed'; path = (Join-Path $env:LOCALAPPDATA 'scantailor-dgi\scantailor-dgi\crashes') } }
$appDirs = @()
if ($AppDir) { $appDirs += $AppDir }
$appDirs += (Join-Path $PSScriptRoot '..')
if ($env:ProgramFiles) { $appDirs += (Join-Path $env:ProgramFiles 'Scantailor-DGI') }
if (${env:ProgramFiles(x86)}) { $appDirs += (Join-Path ${env:ProgramFiles(x86)} 'Scantailor-DGI') }
$seen = @{}
foreach ($a in $appDirs) {
  if (-not (Test-Path -LiteralPath (Join-Path $a 'scantailor.exe'))) { if ($a -ne $AppDir) { continue } }
  $full = $a
  try { $full = (Resolve-Path -LiteralPath $a).ProviderPath } catch { }
  if ($seen.ContainsKey($full.ToLowerInvariant())) { continue }
  $seen[$full.ToLowerInvariant()] = $true
  $sources += @{ tag = ('portable-' + ($seen.Count)); path = (Join-Path $full 'config\crashes'); app = $full }
}
$sources += @{ tag = 'temp-fallback'; path = (Join-Path (Get-DiagTempRoot) 'scantailor-crashes') }

$since = (Get-Date).AddDays(-$Days)
$manifest = [ordered]@{
  tool = 'Collect-Diagnostics ' + (Get-DiagSuiteVersion)
  computer = $computer
  user = [Environment]::UserName
  label = $Label
  collected = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz', $Inv)
  days = $Days
  include_dumps = [bool]$IncludeDumps
  sources = @()
  files = @()
  running_scantailor = @()
}
try {
  foreach ($p in @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'scantailor*' })) {
    $path = $null
    try { $path = $p.Path } catch { }
    $manifest.running_scantailor += [ordered]@{ pid = $p.Id; path = $path; started = $(try { $p.StartTime.ToString('yyyy-MM-ddTHH:mm:ss', $Inv) } catch { $null }) }
  }
} catch { }
if ($manifest.running_scantailor.Count -gt 0) {
  Write-DiagLog 'Scantailor is running: its current log is copied as it is now (the report will show that session without a stop record).' 'warn'
}

$perfRx = '^scantailor-perf-.*\.jsonl$'
$crashRx = '^scantailor-\d{8}-\d{6}-\d+\.(txt|dmp)$'
$stallDumpRx = '^stall-.*\.dmp$'
$total = 0
$totalBytes = [long]0
foreach ($src in $sources) {
  $entry = [ordered]@{ tag = $src.tag; path = $src.path; exists = (Test-Path -LiteralPath $src.path -PathType Container); copied = 0; skipped_old = 0; skipped_dumps = 0 }
  if ($entry.exists) {
    $target = Join-Path $dest ('logs\' + $src.tag)
    foreach ($f in @(Get-ChildItem -LiteralPath $src.path -File -ErrorAction SilentlyContinue)) {
      $isPerf = $f.Name -match $perfRx
      $isLog = $f.Name -like 'scantailor.log*'
      $isCrash = $f.Name -match $crashRx
      $isStallDump = $f.Name -match $stallDumpRx
      if (-not ($isPerf -or $isLog -or $isCrash -or $isStallDump)) { continue }
      if ($f.LastWriteTime -lt $since) { $entry.skipped_old++; continue }
      if ($f.Extension -eq '.dmp' -and -not $IncludeDumps) {
        $entry.skipped_dumps++
        $manifest.files += [ordered]@{ source = $src.tag; name = $f.Name; bytes = $f.Length; modified = $f.LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss', $Inv); copied = $false; note = 'dump not copied (use -IncludeDumps)' }
        continue
      }
      [void][System.IO.Directory]::CreateDirectory($target)
      try {
        Copy-DiagSharedFile -Source $f.FullName -Destination (Join-Path $target $f.Name)
        $entry.copied++
        $total++
        $totalBytes += $f.Length
        $manifest.files += [ordered]@{ source = $src.tag; name = $f.Name; bytes = $f.Length; modified = $f.LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss', $Inv); copied = $true }
      } catch {
        $manifest.files += [ordered]@{ source = $src.tag; name = $f.Name; bytes = $f.Length; copied = $false; note = $_.Exception.Message }
        Write-DiagLog ('Could not copy ' + $f.FullName + ': ' + $_.Exception.Message) 'warn'
      }
    }
  }
  $manifest.sources += $entry
  $state = 'not present'
  if ($entry.exists) { $state = [string]$entry.copied + ' file(s) copied' + $(if ($entry.skipped_old -gt 0) { ', ' + $entry.skipped_old + ' older than ' + $Days + ' days' } else { '' }) + $(if ($entry.skipped_dumps -gt 0) { ', ' + $entry.skipped_dumps + ' dump(s) not copied' } else { '' }) }
  Write-DiagLog ('  ' + $src.path + ': ' + $state)
}
if ($total -eq 0) {
  Write-DiagLog 'No log files found. Diagnostics may be off (SCANTAILOR_DIAG=off or [diagnostics] level=off), the application may not have run in this period, or it uses another location (-AppDir).' 'warn'
}

Write-DiagLog 'Collecting PC inventory ...'
$invPaths = @()
foreach ($s in $sources) { if (Test-Path -LiteralPath $s.path) { $invPaths += $s.path } }
$invPaths += $probeList
try {
  $inv = Get-SystemInventory -Paths $invPaths
  Write-DiagJson -Path (Join-Path $dest 'inventory.json') -InputObject $inv
} catch { Write-DiagLog ('Inventory failed: ' + $_.Exception.Message) 'warn' }

if ($probeList.Count -gt 0) {
  $results = @()
  $results += (Measure-Storage -Path (Get-DiagTempRoot) -Label 'Local temp folder (baseline)' -Role 'workdir' -Quick)
  $i = 0
  foreach ($pd in $probeList) {
    $i++
    Write-DiagLog ('Storage benchmark: ' + $pd + ' ...')
    $r = Measure-Storage -Path $pd -Label ('Probe ' + $i + ': ' + $pd) -Role 'probe' -Quick:$QuickProbe
    $results += $r
    foreach ($h in (Get-StorageHints $r)) { Write-DiagLog ('  ' + $h) 'warn' }
    foreach ($e in @($r.errors)) { if ($e) { Write-DiagLog ('  ' + $e) 'warn' } }
    foreach ($nt in @($r.notes)) { if ($nt) { Write-DiagLog ('  ' + $nt) } }
  }
  Write-DiagJson -Path (Join-Path $dest 'storage.json') -InputObject $results
}

$manifest.total_files = $total
$manifest.total_bytes = $totalBytes
Write-DiagJson -Path (Join-Path $dest 'manifest.json') -InputObject $manifest

$report = Join-Path $dest 'report\report.html'
if (-not $NoAnalyze -and $total -gt 0) {
  Write-DiagLog 'Analysing ...'
  try { & (Join-Path $PSScriptRoot 'Analyze-Diagnostics.ps1') -Path $dest } catch { Write-DiagLog ('Analysis failed: ' + $_.Exception.Message) 'error' }
}

$zip = Join-Path $OutDir ($name + '.zip')
try {
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  try {
    New-DiagZip -Source $dest -Destination $zip
  } catch {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path $dest -DestinationPath $zip -Force
  }
  Write-Host ''
  Write-Host ('Zip to send:   ' + $zip) -ForegroundColor Cyan
} catch {
  Write-DiagLog ('Could not create the zip: ' + $_.Exception.Message) 'error'
}
Write-Host ('Folder:        ' + $dest)
if (Test-Path -LiteralPath $report) {
  Write-Host ('Report:        ' + $report) -ForegroundColor Cyan
  if (-not $NoOpen -and [Environment]::UserInteractive -and -not ($env:CI -or $env:GITHUB_ACTIONS -or $env:TF_BUILD)) { try { Start-Process -FilePath $report } catch { } }
}
