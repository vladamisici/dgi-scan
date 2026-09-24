<#
.SYNOPSIS
  Puts two or more diagnostics reports side by side (for example PC Dani and
  PC Milena) in one HTML page.

.DESCRIPTION
  Each argument is a summary.json, a folder containing one (or a report\
  subfolder with one), or a zip made by Collect-Diagnostics. The page shows
  the verdicts, the environment with differences highlighted, the storage
  benchmark, GUI stalls, stage throughput, page-load latency, memory growth and
  the slowest GUI-thread operations. For numbers, the worst value is marked in
  red when it is more than 1.5 times the best.

.EXAMPLE
  .\Compare-Reports.ps1 C:\stress\dani C:\stress\milena
.EXAMPLE
  .\Compare-Reports.ps1 -Path diag-PC1-20260923-1700.zip, diag-PC2-20260923-1705.zip -Out C:\temp\compare.html
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Path,
  [string]$Out,
  [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
try {
  Get-ChildItem -LiteralPath $PSScriptRoot -File -ErrorAction Stop |
    Where-Object { $_.Extension -match '^\.(ps1|psm1|cmd)$' } | Unblock-File -ErrorAction SilentlyContinue
} catch { }
Import-Module (Join-Path $PSScriptRoot 'DiagnosticsCommon.psm1') -DisableNameChecking
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

$inputs = @()
foreach ($v in @($Path)) { foreach ($p in ($v -split ';')) { if ($p.Trim()) { $inputs += $p.Trim().Trim('"') } } }
if ($inputs.Count -lt 2) {
  Write-Host 'Usage: Compare-Reports.ps1 <report 1> <report 2> [...] [-Out compare.html]'
  Write-Host '  Each report is a summary.json, a folder with one (or with report\summary.json), or a Collect-Diagnostics zip.'
  exit 1
}

function Read-Summary([string]$p) {
  if (-not (Test-Path -LiteralPath $p)) { throw ('Not found: ' + $p) }
  $item = Get-Item -LiteralPath $p
  if ($item.PSIsContainer) {
    foreach ($c in @((Join-Path $item.FullName 'summary.json'), (Join-Path $item.FullName 'report\summary.json'))) {
      if (Test-Path -LiteralPath $c) { return @{ s = (Read-DiagJson -Path $c); src = $c; folder = $item.Name } }
    }
    $found = Get-ChildItem -LiteralPath $item.FullName -Recurse -Filter 'summary.json' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return @{ s = (Read-DiagJson -Path $found.FullName); src = $found.FullName; folder = $item.Name } }
    throw ('No summary.json in ' + $p + ' - run Analyze-Diagnostics.ps1 on it first.')
  }
  if ($item.Extension -eq '.zip') {
    try { Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop } catch { }
    $zip = [System.IO.Compression.ZipFile]::OpenRead($item.FullName)
    try {
      $entry = $zip.Entries | Where-Object { $_.FullName -match '(^|/)report/summary\.json$' -or $_.Name -eq 'summary.json' } | Select-Object -First 1
      if (-not $entry) { throw ('No summary.json inside ' + $p) }
      $sr = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
      try { $text = $sr.ReadToEnd() } finally { $sr.Dispose() }
      return @{ s = (ConvertFrom-Json -InputObject $text); src = ($item.FullName + '!' + $entry.FullName); folder = $item.BaseName }
    } finally { $zip.Dispose() }
  }
  $folder = Split-Path -Leaf $item.DirectoryName
  if ($folder -eq 'report') { $folder = Split-Path -Leaf (Split-Path -Parent $item.DirectoryName) }
  return @{ s = (Read-DiagJson -Path $item.FullName); src = $item.FullName; folder = $folder }
}

$reports = @()
foreach ($p in $inputs) {
  try {
    $r = Read-Summary $p
    $lbl = [string]$r.s.label
    if (-not $lbl) { $lbl = [string]$r.s.computer }
    if (-not $lbl) { $lbl = $r.folder }
    $base = $lbl; $i = 2
    while (@($reports | Where-Object { $_.label -eq $lbl }).Count -gt 0) { $lbl = $base + ' (' + $i + ')'; $i++ }
    $r.label = $lbl
    $reports += $r
    Write-DiagLog ('Loaded ' + $lbl + ' <- ' + $r.src)
  } catch {
    Write-DiagLog $_.Exception.Message 'error'
    exit 1
  }
}

# ---------------------------------------------------------------------------
# Row builders. Each row is a label plus one value per report.
# ---------------------------------------------------------------------------

function Get-Col([scriptblock]$Get) {
  $vals = @()
  foreach ($r in $reports) {
    $v = $null
    try { $v = & $Get $r.s } catch { $v = $null }
    $vals += , $v
  }
  return , $vals
}

$sections = New-Object System.Collections.Generic.List[string]
$headers = @('') + @($reports | ForEach-Object { $_.label })

function New-CompareTable([string]$Title, $Rows, [string]$Note = '', [switch]$Text) {
  $html = ''
  if ($Title) { $html += '<h2>' + (ConvertTo-DiagHtml $Title) + '</h2>' }
  if ($Note) { $html += '<p class="muted">' + (ConvertTo-DiagHtml $Note) + '</p>' }
  $num = @()
  if (-not $Text) { $num = @(1..$reports.Count) }
  $html += New-DiagHtmlTable -Headers $headers -Rows $Rows -NumericColumns $num -Class 'cmp'
  return $html
}

function Add-TextRow($rows, [string]$label, $vals) {
  # Environment-style row: highlighted when the reports differ.
  $strs = @($vals | ForEach-Object { if ($null -eq $_) { '-' } elseif ($_ -is [bool]) { ([string]$_).ToLowerInvariant() } else { [string]$_ } })
  $distinct = @($strs | Where-Object { $_ -ne '-' } | Sort-Object -Unique)
  $row = @($label)
  foreach ($s in $strs) {
    if ($distinct.Count -gt 1) { $row += @{ text = $s; cls = 'diff' } } else { $row += $s }
  }
  $rows.Add($row)
}

function Add-NumRow($rows, [string]$label, $vals, [string]$fmt = 'ms', [switch]$HigherIsBetter, [int]$Decimals = 1, [switch]$NoRank) {
  # Numeric row: the worst value is red and the best green when they differ
  # by more than half.
  $nums = @()
  foreach ($v in $vals) { if ($null -ne $v -and "$v" -ne '') { try { $nums += [double]$v } catch { } } }
  $best = $null; $worst = $null
  if ($nums.Count -ge 2) {
    $mn = ($nums | Measure-Object -Minimum).Minimum
    $mx = ($nums | Measure-Object -Maximum).Maximum
    $lo = $mn; $hi = $mx
    if ($HigherIsBetter) { $best = $mx; $worst = $mn } else { $best = $mn; $worst = $mx }
    $ratio = 0.0
    if ($lo -gt 0) { $ratio = $hi / $lo } elseif ($hi -gt 0) { $ratio = [double]::PositiveInfinity }
    if ($ratio -le 1.5 -or $NoRank) { $best = $null; $worst = $null }
  }
  $row = @($label)
  foreach ($v in $vals) {
    if ($null -eq $v -or "$v" -eq '') { $row += '-'; continue }
    $d = [double]$v
    $txt = ''
    switch ($fmt) {
      'ms' { $txt = Format-DiagMs $d }
      'mb' { $txt = Format-DiagMB $d }
      'pct' { $txt = (Format-DiagNumber $d 2) + '%' }
      default { $txt = Format-DiagNumber $d $Decimals }
    }
    $cls = ''
    if ($null -ne $worst -and $d -eq $worst) { $cls = 'bad' } elseif ($null -ne $best -and $d -eq $best) { $cls = 'good' }
    if ($cls) { $row += @{ text = $txt; cls = $cls } } else { $row += $txt }
  }
  $rows.Add($row)
}

# 1. Verdicts
$rows = New-Object System.Collections.Generic.List[object]
Add-TextRow $rows 'Computer' (Get-Col { param($s) $s.computer })
Add-TextRow $rows 'Kind' (Get-Col { param($s) $s.kind })
Add-TextRow $rows 'Complete run' (Get-Col { param($s) if ([string]$s.kind -eq 'stress' -and $s.PSObject.Properties['aborted']) { if ($s.aborted) { 'no - aborted, results partial' } elseif ($s.partial_note) { 'no - runner did not finish' } else { 'yes' } } })
Add-TextRow $rows 'Generated' (Get-Col { param($s) Format-DiagWhen $s.generated })
$vrow = @('Overall verdict')
foreach ($r in $reports) { $vrow += @{ html = (Get-DiagStatusBadge ([string]$r.s.verdict.overall)) } }
$rows.Add($vrow)
$checkIds = @()
foreach ($r in $reports) { foreach ($c in @($r.s.verdict.checks)) { if ($checkIds -notcontains [string]$c.id) { $checkIds += [string]$c.id } } }
foreach ($id in $checkIds) {
  $name = $id
  $row = @()
  foreach ($r in $reports) {
    $c = @($r.s.verdict.checks) | Where-Object { [string]$_.id -eq $id } | Select-Object -First 1
    if ($c) { $name = [string]$c.name; $row += @{ html = (Get-DiagStatusBadge ([string]$c.status)) + ' <span class="muted">' + (ConvertTo-DiagHtml $c.summary) + '</span>' } } else { $row += '-' }
  }
  $rows.Add(@($name) + $row)
}
Add-NumRow $rows 'Processes analysed' (Get-Col { param($s) @($s.instances).Count }) 'n' -Decimals 0 -NoRank
Add-NumRow $rows 'Total run time' (Get-Col { param($s) $t = 0.0; foreach ($i in @($s.instances)) { $t += [double]$i.duration_s }; $t * 1000 }) 'ms' -NoRank
$sections.Add((New-CompareTable 'Verdicts' $rows -Text))

# 2. Environment
$rows = New-Object System.Collections.Generic.List[object]
Add-TextRow $rows 'Windows' (Get-Col { param($s) if ($s.environment.inventory.os) { [string]$s.environment.inventory.os.caption + ' ' + [string]$s.environment.inventory.os.display_version + ' (' + [string]$s.environment.inventory.os.build + ')' } else { [string]$s.environment.start.os + ' (' + [string]$s.environment.start.os_kernel + ')' } })
Add-TextRow $rows 'Application version' (Get-Col { param($s) [string]$s.environment.start.version })
Add-TextRow $rows 'Diagnostics level' (Get-Col { param($s) [string]$s.environment.start.level })
Add-TextRow $rows 'CPU' (Get-Col { param($s) if ($s.environment.inventory.cpu) { $c = @($s.environment.inventory.cpu)[0]; [string]$c.name + ' (' + [string]$c.cores + 'C/' + [string]$c.logical + 'T)' } else { [string]$s.environment.start.cpu + ' (' + [string]$s.environment.start.cores + ' threads)' } })
Add-TextRow $rows 'RAM' (Get-Col { param($s) Format-DiagMB $s.environment.start.ram_mb })
Add-TextRow $rows 'Model' (Get-Col { param($s) if ($s.environment.inventory.system) { [string]$s.environment.inventory.system.manufacturer + ' ' + [string]$s.environment.inventory.system.model } })
Add-TextRow $rows 'Disks' (Get-Col { param($s) (@($s.environment.inventory.physical_disks) | Where-Object { $_ } | ForEach-Object { [string]$_.media_type + ' ' + [string]$_.bus_type }) -join ', ' })
Add-TextRow $rows 'Work/log folder on' (Get-Col { param($s) $pth = @($s.environment.inventory.paths) | Where-Object { $_ } | Select-Object -First 1; if ($pth) { if ($pth.network) { 'network ' + [string]$pth.target } else { [string]$pth.drive_type + ' ' + [string]$pth.media_type } } })
Add-TextRow $rows 'Network links' (Get-Col { param($s) (@($s.environment.inventory.network_adapters) | Where-Object { $_ } | ForEach-Object { [string]$_.link_speed }) -join ', ' })
Add-TextRow $rows 'Power plan' (Get-Col { param($s) [string]$s.environment.inventory.power.name })
Add-TextRow $rows 'Defender real-time' (Get-Col { param($s) if ($null -ne $s.environment.inventory.defender.realtime) { if ($s.environment.inventory.defender.realtime) { 'on' } else { 'off' } } })
Add-TextRow $rows 'Antivirus products' (Get-Col { param($s) (@($s.environment.inventory.antivirus_products) | Where-Object { $_ } | ForEach-Object { [string]$_.name }) -join ', ' })
Add-TextRow $rows 'Other agents running' (Get-Col { param($s) (@($s.environment.inventory.processes) | Where-Object { $_ -and $_.category -ne 'scantailor' } | ForEach-Object { [string]$_.name }) -join ', ' })
Add-TextRow $rows 'Display adapters' (Get-Col { param($s) (@($s.environment.inventory.display_adapters) | Where-Object { $_ } | ForEach-Object { [string]$_.name + ' ' + [string]$_.driver_version }) -join '; ' })
Add-TextRow $rows 'Screens (logical size @ pixel ratio)' (Get-Col { param($s) (@($s.environment.screens) | Where-Object { $_ } | ForEach-Object { [string]$_.w + 'x' + [string]$_.h + ' @' + [string]$_.dpr }) -join ', ' })
Add-TextRow $rows 'Uptime at collection (h)' (Get-Col { param($s) $s.environment.inventory.os.uptime_hours })
Add-TextRow $rows '%TEMP% files' (Get-Col { param($s) if ($s.environment.inventory.temp) { [string]$s.environment.inventory.temp.files + $(if (-not $s.environment.inventory.temp.complete) { '+' } else { '' }) } })
# Batch threads: batch_threads is the effective count in newer builds, which
# also log the stored value (batch_threads_setting) and the worker priority.
Add-TextRow $rows 'Batch threads (effective)' (Get-Col { param($s) $st = $s.environment.settings; if ($st -and $null -ne $st.batch_threads) { if ($st.PSObject.Properties['batch_threads_setting']) { [string]$st.batch_threads } else { [string]$st.batch_threads + ' (older build: stored value)' } } })
Add-TextRow $rows 'Batch threads (setting)' (Get-Col { param($s) $st = $s.environment.settings; if ($st -and $st.PSObject.Properties['batch_threads_setting']) { [string]$st.batch_threads_setting } })
Add-TextRow $rows 'Worker thread priority' (Get-Col { param($s) $st = $s.environment.settings; if ($st -and $st.worker_thread_priority) { [string]$st.worker_thread_priority } })
$tiffNames = @{ 1 = 'none'; 2 = 'CCITT RLE'; 3 = 'CCITT G3'; 4 = 'CCITT G4'; 5 = 'LZW'; 6 = 'old JPEG'; 7 = 'JPEG'; 8 = 'Deflate'; 32773 = 'PackBits'; 32946 = 'Deflate (old code)' }
Add-TextRow $rows 'Generated pages' (Get-Col { param($s)
    $g = $s.environment.generate
    if ($g) {
      $txt = [string]$g.count + ' x ' + [string]$g.kind + ' @ ' + [string]$g.dpi + ' dpi'
      if ($null -ne $g.compression) {
        $nm = $tiffNames[[int]$g.compression]
        if ($nm) { $txt += ', ' + $nm + ' (' + [string]$g.compression + ')' } else { $txt += ', compression ' + [string]$g.compression }
      }
      $txt
    }
  })
$keys = @()
$shown = @('t', 'ev', 'th', 'tid', 'batch_threads', 'batch_threads_setting', 'worker_thread_priority')
foreach ($r in $reports) {
  $st = $r.s.environment.settings
  if ($st) { foreach ($pp in $st.PSObject.Properties) { if ($keys -notcontains $pp.Name -and $shown -notcontains $pp.Name) { $keys += $pp.Name } } }
}
foreach ($k in $keys) { Add-TextRow $rows ('Setting: ' + $k) (Get-Col ([scriptblock]::Create('param($s) $s.environment.settings.''' + $k.Replace("'", "''") + '''' ))) }
$sections.Add((New-CompareTable 'Environment' $rows 'Rows where the PCs differ are highlighted.' -Text))

# 3. Storage
$rows = New-Object System.Collections.Generic.List[object]
function Get-Probe($s, [string]$role, [int]$idx) {
  $list = @(@($s.storage) | Where-Object { $_ -and [string]$_.role -eq $role })
  if ($list.Count -gt $idx) { return $list[$idx] }
  return $null
}
$probeSlots = @(@{ role = 'workdir'; idx = 0; name = 'Work folder' }, @{ role = 'scans'; idx = 0; name = 'Scans folder' })
$maxProbe = 0
foreach ($r in $reports) { $n = @(@($r.s.storage) | Where-Object { $_ -and [string]$_.role -eq 'probe' }).Count; if ($n -gt $maxProbe) { $maxProbe = $n } }
for ($i = 0; $i -lt $maxProbe; $i++) { $probeSlots += @{ role = 'probe'; idx = $i; name = ('Probe ' + ($i + 1)) } }
foreach ($slot in $probeSlots) {
  $present = $false
  foreach ($r in $reports) { if (Get-Probe $r.s $slot.role $slot.idx) { $present = $true } }
  if (-not $present) { continue }
  $role = $slot.role; $idx = $slot.idx
  Add-TextRow $rows ($slot.name + ': path') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr) { [string]$pr.path } })
  Add-NumRow $rows ($slot.name + ': create 4 KB p95') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr -and $pr.tests.small_create) { $pr.tests.small_create.p95 } }) 'ms'
  Add-NumRow $rows ($slot.name + ': stat p50') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr -and $pr.tests.stat) { $pr.tests.stat.p50 } }) 'ms'
  Add-NumRow $rows ($slot.name + ': durable 1 MB p50') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr -and $pr.tests.durable_1mb) { $pr.tests.durable_1mb.p50 } }) 'ms'
  Add-NumRow $rows ($slot.name + ': durable 1 MB p95') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr -and $pr.tests.durable_1mb) { $pr.tests.durable_1mb.p95 } }) 'ms'
  Add-NumRow $rows ($slot.name + ': rename p95') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr -and $pr.tests.rename) { $pr.tests.rename.p95 } }) 'ms'
  Add-NumRow $rows ($slot.name + ': sequential write MB/s') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr -and $pr.tests.seq_write) { $pr.tests.seq_write.mb_per_s } }) 'n' -HigherIsBetter
  Add-NumRow $rows ($slot.name + ': sequential read MB/s') (Get-Col { param($s) $pr = Get-Probe $s $role $idx; if ($pr -and $pr.tests.seq_read) { $pr.tests.seq_read.mb_per_s } }) 'n' -HigherIsBetter
}
if ($rows.Count -gt 0) { $sections.Add((New-CompareTable 'Storage benchmark' $rows 'Measured by the scripts outside the application. Lower times and higher MB/s are better.')) }

# 4. GUI stalls
$rows = New-Object System.Collections.Generic.List[object]
function Get-StallTotals($s) {
  # Rates over the run time of EVERY process, stall-free ones included: a
  # process without stalls adds run time and no stalls, and leaving it out
  # would overstate the share stalled and the stalls per hour.
  $t = @{ count = 0; total = 0.0; longest = 0.0; over1 = 0; run = 0.0; pct = $null; per_hour = $null }
  foreach ($i in @($s.instances)) {
    $t.count += [int]$i.stalls.count; $t.total += [double]$i.stalls.total_ms; $t.over1 += [int]$i.stalls.over_1s
    if ([double]$i.stalls.longest_ms -gt $t.longest) { $t.longest = [double]$i.stalls.longest_ms }
    $r = $null
    # Newer summaries store the denominator; older ones only let it be
    # recovered from total/pct (when there were stalls) or the log duration.
    if ($i.stalls.PSObject.Properties['run_ms'] -and $null -ne $i.stalls.run_ms) { $r = [double]$i.stalls.run_ms }
    elseif ($null -ne $i.stalls.pct_of_run -and [double]$i.stalls.pct_of_run -gt 0) { $r = [double]$i.stalls.total_ms * 100.0 / [double]$i.stalls.pct_of_run }
    elseif ($null -ne $i.duration_s) { $r = [double]$i.duration_s * 1000.0 }
    if ($null -ne $r -and $r -gt 0) { $t.run += $r }
  }
  if ($t.run -gt 0) { $t.pct = 100.0 * $t.total / $t.run; $t.per_hour = $t.count / ($t.run / 3600000.0) }
  # The GUI check of the report has the same share, computed from the
  # process logs themselves; it is the authoritative figure when present.
  $gui = @($s.verdict.checks) | Where-Object { $_ -and [string]$_.id -eq 'gui' } | Select-Object -First 1
  if ($gui -and $gui.numbers) {
    if ($null -ne $gui.numbers.pct) { $t.pct = [double]$gui.numbers.pct }
    if ($gui.numbers.PSObject.Properties['run_ms'] -and [double]$gui.numbers.run_ms -gt 0) { $t.per_hour = [double]$gui.numbers.count / ([double]$gui.numbers.run_ms / 3600000.0) }
  }
  return $t
}
Add-NumRow $rows 'Stalls counted' (Get-Col { param($s) (Get-StallTotals $s).count }) 'n' -Decimals 0 -NoRank
Add-NumRow $rows 'Stalls per hour' (Get-Col { param($s) (Get-StallTotals $s).per_hour }) 'n'
Add-NumRow $rows 'Total stalled time' (Get-Col { param($s) (Get-StallTotals $s).total }) 'ms' -NoRank
Add-NumRow $rows 'Longest stall' (Get-Col { param($s) (Get-StallTotals $s).longest }) 'ms'
Add-NumRow $rows 'Stalls over 1 s' (Get-Col { param($s) (Get-StallTotals $s).over1 }) 'n' -Decimals 0 -NoRank
Add-NumRow $rows '% of run time stalled' (Get-Col { param($s) (Get-StallTotals $s).pct }) 'pct'
Add-TextRow $rows 'Top operations in stalls (innermost)' (Get-Col { param($s) (@($s.stalls.by_innermost_op) | Select-Object -First 3 | ForEach-Object { [string]$_.op + ' ' + (Format-DiagMs $_.ms) }) -join '; ' })
$sections.Add((New-CompareTable 'GUI stalls' $rows 'Only stalls counted in the verdict (not startup, shutdown or test-driver stalls). "% of run time stalled" uses the run phase of each process.'))

# 5. Stage throughput and page loads
$rows = New-Object System.Collections.Generic.List[object]
$stageNames = @('Fix orientation', 'Split pages', 'Deskew', 'Select content', 'Margins', 'Output')
function Test-MixedOutputBatch($s) {
  # Summaries from before re-check runs were kept apart (no stages.batch_recheck)
  # average reopen-mode Output re-checks of finished output into Output.
  if ($s.stages -and $s.stages.PSObject.Properties['batch_recheck']) { return $false }
  if ($s.run -and $s.run.params -and [string]$s.run.params.Mode -eq 'reopen' -and [int]$s.run.params.Cycles -gt 1) { return $true }
  return $false
}
$mixedOld = @($reports | Where-Object { Test-MixedOutputBatch $_.s } | ForEach-Object { $_.label })
for ($st = 0; $st -lt 6; $st++) {
  $sv = $st
  $vals = Get-Col { param($s) if ($sv -eq 5 -and (Test-MixedOutputBatch $s)) { return $null }; $b = @($s.stages.batch) | Where-Object { $_ -and [int]$_.stage -eq $sv } | Select-Object -First 1; if ($b) { $b.mean } }
  if (@($vals | Where-Object { $null -ne $_ }).Count -gt 0) { Add-NumRow $rows ('Batch s/page: ' + $stageNames[$st]) $vals 'n' -Decimals 3 }
}
$vals = Get-Col { param($s) $b = @($s.stages.batch_recheck) | Where-Object { $_ -and [int]$_.stage -eq 5 } | Select-Object -First 1; if ($b) { $b.mean } }
if (@($vals | Where-Object { $null -ne $_ }).Count -gt 0) { Add-NumRow $rows 'Output re-check of finished output, s/page (reopen mode)' $vals 'n' -Decimals 3 }
for ($st = 0; $st -lt 6; $st++) {
  $sv = $st
  $vals = Get-Col { param($s) $b = @($s.stages.self_ms) | Where-Object { $_ -and [int]$_.stage -eq $sv } | Select-Object -First 1; if ($b) { $b.p50 } }
  if (@($vals | Where-Object { $null -ne $_ }).Count -gt 0) { Add-NumRow $rows ('Stage own time p50: ' + $stageNames[$st]) $vals 'ms' }
}
for ($st = 0; $st -lt 6; $st++) {
  $sv = $st
  $vals = Get-Col { param($s) $b = @($s.page_load) | Where-Object { $_ -and [int]$_.stage -eq $sv } | Select-Object -First 1; if ($b) { $b.p95 } }
  if (@($vals | Where-Object { $null -ne $_ }).Count -gt 0) { Add-NumRow $rows ('Page load p95: ' + $stageNames[$st]) $vals 'ms' }
}
$psNote = 'Batch seconds per page (stress runs; re-checks of already finished output in reopen mode are a separate row, not part of Output), own time of each stage per page, and time from selecting a page until it is shown.'
if ($mixedOld.Count -gt 0) { $psNote += ' Batch Output is left out for ' + ($mixedOld -join ', ') + ': that summary comes from an older analyzer that averaged the re-checks into it; run Analyze-Diagnostics.ps1 on that folder again to include it.' }
if ($rows.Count -gt 0) { $sections.Add((New-CompareTable 'Processing speed' $rows $psNote)) }

# 6. Memory and resources
$rows = New-Object System.Collections.Generic.List[object]
foreach ($m in @(@{ k = 'private_mb'; n = 'Private memory MB/cycle' }, @{ k = 'handles'; n = 'Handles/cycle' }, @{ k = 'gdi'; n = 'GDI/cycle' }, @{ k = 'user'; n = 'USER/cycle' }, @{ k = 'threads'; n = 'Threads/cycle' })) {
  $mk = $m.k
  $vals = Get-Col { param($s) $mx = $null; foreach ($i in @($s.instances)) { foreach ($e in @($i.leak)) { if ($e -and [string]$e.metric -eq $mk -and $null -ne $e.slope) { if ($null -eq $mx -or [double]$e.slope -gt $mx) { $mx = [double]$e.slope } } } }; $mx }
  if (@($vals | Where-Object { $null -ne $_ }).Count -gt 0) { Add-NumRow $rows ($m.n + ' (steepest)') $vals 'n' -Decimals 2 }
}
Add-NumRow $rows 'Peak working set' (Get-Col { param($s) $mx = $null; foreach ($i in @($s.instances)) { if ($null -ne $i.peak_ws_mb -and ($null -eq $mx -or [double]$i.peak_ws_mb -gt $mx)) { $mx = [double]$i.peak_ws_mb } }; $mx }) 'mb'
Add-NumRow $rows 'Private memory trend MB/hour (everyday logs)' (Get-Col { param($s) $mx = $null; foreach ($i in @($s.instances)) { if ($i.trend -and ($null -eq $mx -or [double]$i.trend.mb_per_hour -gt $mx)) { $mx = [double]$i.trend.mb_per_hour } }; $mx }) 'n'
$sections.Add((New-CompareTable 'Memory and resources' $rows))

# 7. GUI-thread operations
$rows = New-Object System.Collections.Generic.List[object]
$opNames = @{}
foreach ($r in $reports) {
  foreach ($o in (@($r.s.ops) | Where-Object { $_ -and [string]$_.th -eq 'gui' } | Sort-Object { [double]$_.max } -Descending | Select-Object -First 12)) { $opNames[[string]$o.name] = $true }
}
foreach ($nm in ($opNames.Keys | Sort-Object)) {
  $n2 = $nm
  Add-NumRow $rows ($nm + ' p95') (Get-Col { param($s) $o = @($s.ops) | Where-Object { $_ -and [string]$_.th -eq 'gui' -and [string]$_.name -eq $n2 } | Select-Object -First 1; if ($o) { $o.p95 } }) 'ms'
  Add-NumRow $rows ($nm + ' max') (Get-Col { param($s) $o = @($s.ops) | Where-Object { $_ -and [string]$_.th -eq 'gui' -and [string]$_.name -eq $n2 } | Select-Object -First 1; if ($o) { $o.max } }) 'ms'
}
if ($rows.Count -gt 0) { $sections.Add((New-CompareTable 'Slowest GUI-thread operations' $rows 'Every call on the GUI thread freezes the window for its duration. In basic-level logs only slow calls are recorded, so percentiles there describe slow calls only.')) }

# Sources
$src = '<h2>Sources</h2><ul>'
foreach ($r in $reports) { $src += '<li><b>' + (ConvertTo-DiagHtml $r.label) + '</b>: <span class="mono">' + (ConvertTo-DiagHtml $r.src) + '</span></li>' }
$src += '</ul>'
$sections.Add($src)

if (-not $Out) {
  $desk = [Environment]::GetFolderPath('Desktop')
  if (-not $desk -or -not (Test-Path -LiteralPath $desk)) { $desk = (Get-Location).ProviderPath }
  $Out = Join-Path $desk ('compare-' + (Get-Date).ToString('yyyyMMdd-HHmm', $Inv) + '.html')
}
# A relative -Out is relative to PowerShell's current location; the .NET call
# that writes the file would resolve it against the process directory.
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
$title = 'Scantailor-DGI comparison: ' + (($reports | ForEach-Object { $_.label }) -join ' vs ')
$doc = '<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>' +
(ConvertTo-DiagHtml $title) + '</title><style>' + (Get-DiagReportCss) + '</style></head><body><main><h1>' + (ConvertTo-DiagHtml $title) +
'</h1><div class="sub">Generated ' + (Get-Date).ToString('yyyy-MM-dd HH:mm', $Inv) + '. Red: worst value when it is more than 1.5 times the best; green: best. Yellow rows: the environments differ.</div>' +
($sections -join '') + '</main></body></html>'
$outDir = Split-Path -Parent $Out
if ($outDir) { [void][System.IO.Directory]::CreateDirectory($outDir) }
Write-DiagText -Path $Out -Text $doc
Write-Host ('Comparison: ' + $Out) -ForegroundColor Cyan
if (-not $NoOpen -and [Environment]::UserInteractive -and -not ($env:CI -or $env:GITHUB_ACTIONS -or $env:TF_BUILD)) { try { Start-Process -FilePath $Out } catch { } }
