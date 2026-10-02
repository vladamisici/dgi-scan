# DiagnosticsCommon.psm1 - shared helpers for the Scantailor-DGI diagnostics suite.
#
# Works in Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII-only:
# Windows PowerShell reads BOM-less scripts in the ANSI code page, so any other
# character would be mangled on a Romanian Windows.
#
# Nothing here needs administrator rights. Every probe of the machine is best
# effort: a probe that fails records why and the rest carries on, because a
# half-filled inventory from an operator PC is still worth far more than none.

Set-StrictMode -Off

$script:Inv = [System.Globalization.CultureInfo]::InvariantCulture
$script:SerializerTried = $false
$script:Serializer = $null
$script:NativeTried = $false
$script:NativeOk = $false

# Categorical series colours, in fixed order (validated palette: adjacent pairs
# stay distinguishable for the common colour-vision deficiencies).
$script:Palette = @('#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#4a3aa7', '#e34948')

function Get-DiagPalette {
  return , $script:Palette
}

function Get-DiagSuiteVersion {
  return '1.0'
}

# ---------------------------------------------------------------------------
# Small utilities
# ---------------------------------------------------------------------------

function Write-DiagLog {
  param([string]$Message, [ValidateSet('info', 'warn', 'error', 'ok')][string]$Level = 'info')
  $stamp = (Get-Date).ToString('HH:mm:ss', $script:Inv)
  $color = 'Gray'
  if ($Level -eq 'warn') { $color = 'Yellow' }
  elseif ($Level -eq 'error') { $color = 'Red' }
  elseif ($Level -eq 'ok') { $color = 'Green' }
  Write-Host ('[' + $stamp + '] ' + $Message) -ForegroundColor $color
}

function Get-DiagTempRoot {
  # $env:TEMP is absent under pwsh on Linux CI runners.
  $t = $env:TEMP
  if (-not $t) { $t = [System.IO.Path]::GetTempPath() }
  return $t
}

function Get-DiagComputerName {
  $n = $env:COMPUTERNAME
  if (-not $n) { $n = [System.Environment]::MachineName }
  return $n
}

function Test-DiagIsWindows {
  if ($PSVersionTable.PSVersion.Major -lt 6) { return $true }
  return [bool]$IsWindows
}

function Write-DiagJson {
  <# Writes an object as UTF-8 JSON without a BOM (Qt's JSON parser rejects a BOM). #>
  param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$InputObject, [int]$Depth = 12)
  $json = ConvertTo-Json -InputObject $InputObject -Depth $Depth
  [System.IO.File]::WriteAllText($Path, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Read-DiagJson {
  param([Parameter(Mandatory = $true)][string]$Path)
  $text = [System.IO.File]::ReadAllText($Path)
  return (ConvertFrom-Json -InputObject $text)
}

function Write-DiagText {
  param([Parameter(Mandatory = $true)][string]$Path, [string]$Text)
  [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-DiagNaturalKey {
  <# Sort key that orders "page2" before "page10", like the application does. #>
  param([string]$Name)
  return [regex]::Replace($Name.ToLowerInvariant(), '\d+', { param($m) $m.Value.PadLeft(12, '0') })
}

function Test-DiagAsciiPath {
  param([string]$Path)
  return -not ($Path -match '[^\x20-\x7E]')
}

function Copy-DiagSharedFile {
  <#
    Copies a file that another process may still have open for writing (the
    application keeps its current log open). Copy-Item asks for exclusive
    read sharing and fails with a sharing violation in that case.
  #>
  param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
  $in = $null
  $out = $null
  try {
    $in = [System.IO.FileStream]::new($Source, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
      [System.IO.FileShare]'ReadWrite, Delete', 1048576)
    $out = [System.IO.FileStream]::new($Destination, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
      [System.IO.FileShare]::None, 1048576)
    $in.CopyTo($out, 1048576)
  } finally {
    if ($out) { $out.Dispose() }
    if ($in) { $in.Dispose() }
  }
  try { [System.IO.File]::SetLastWriteTimeUtc($Destination, [System.IO.File]::GetLastWriteTimeUtc($Source)) } catch { }
}

function New-DiagZip {
  <#
    Zips $Source (a folder) into $Destination, the folder itself as the top
    entry. Entry names use '/', which every unzip tool reads; .NET Framework's
    CreateFromDirectory writes backslashes there.
  #>
  param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
  Add-Type -AssemblyName System.IO.Compression -ErrorAction SilentlyContinue
  Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
  $src = (Resolve-Path -LiteralPath $Source).ProviderPath.TrimEnd([char]92, [char]47)
  $parent = Split-Path -Parent $src
  $fs = [System.IO.FileStream]::new($Destination, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
  try {
    $zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create, $true)
    try {
      foreach ($f in @(Get-ChildItem -LiteralPath $src -Recurse -File)) {
        $rel = $f.FullName.Substring($parent.Length).TrimStart([char]92, [char]47).Replace([char]92, [char]47)
        $entry = $zip.CreateEntry($rel, [System.IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [datetimeoffset]$f.LastWriteTime
        $in = [System.IO.FileStream]::new($f.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete')
        $out = $entry.Open()
        try { $in.CopyTo($out) } finally { $out.Dispose(); $in.Dispose() }
      }
    } finally { $zip.Dispose() }
  } finally { $fs.Dispose() }
}

function Initialize-DiagNative {
  <#
    Compiles the few Win32 calls the suite needs. Add-Type can be unavailable
    (locked-down machines); every caller has a fallback, so failure is fine.
  #>
  if ($script:NativeTried) { return $script:NativeOk }
  $script:NativeTried = $true
  if (-not (Test-DiagIsWindows)) { return $false }
  try {
    if (-not ('StDiag.Native' -as [type])) {
      Add-Type -Namespace StDiag -Name Native -ErrorAction Stop -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string existingFileName, string newFileName, int flags);
[DllImport("user32.dll")]
public static extern uint GetGuiResources(IntPtr process, uint flags);
[DllImport("user32.dll")]
public static extern bool SetProcessDPIAware();
'@
    }
    $script:NativeOk = $true
  } catch {
    $script:NativeOk = $false
  }
  return $script:NativeOk
}

# ---------------------------------------------------------------------------
# JSON lines reading
#
# One ConvertFrom-Json call per line costs far too much in Windows PowerShell
# (a 1M-line log would take the better part of an hour). Instead the file is
# read in ~1 MB chunks, the complete lines of a chunk are joined into one JSON
# array and parsed in a single call: JavaScriptSerializer in 5.1, ConvertFrom-
# Json in 7 (which is fast there, and JavaScriptSerializer does not exist).
# A chunk that fails to parse as a whole is re-parsed line by line so that a
# damaged line costs only itself.
# ---------------------------------------------------------------------------

function Get-DiagJsonSerializer {
  if ($script:SerializerTried) { return $script:Serializer }
  $script:SerializerTried = $true
  if ($PSVersionTable.PSVersion.Major -lt 6) {
    try {
      Add-Type -AssemblyName System.Web.Extensions -ErrorAction Stop
      $s = New-Object System.Web.Script.Serialization.JavaScriptSerializer
      $s.MaxJsonLength = [int]::MaxValue
      $s.RecursionLimit = 64
      $script:Serializer = $s
    } catch {
      $script:Serializer = $null
    }
  }
  return $script:Serializer
}

function ConvertFrom-DiagJsonArrayText {
  # Returns object[]; throws when the text is not valid JSON.
  param([string]$Text)
  $ser = Get-DiagJsonSerializer
  if ($null -ne $ser) {
    $arr = $ser.DeserializeObject($Text)
  } else {
    $arr = ConvertFrom-Json -InputObject $Text
  }
  if ($null -eq $arr) { return , @() }
  if ($arr -isnot [array]) { $arr = @($arr) }
  return , $arr
}

function ConvertFrom-DiagJsonLine {
  param([string]$Text)
  $ser = Get-DiagJsonSerializer
  if ($null -ne $ser) { return $ser.DeserializeObject($Text) }
  return (ConvertFrom-Json -InputObject $Text)
}

function Open-JsonlReader {
  param([Parameter(Mandatory = $true)][string]$Path, [int]$ChunkChars = 1048576)
  $fs = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
    [System.IO.FileShare]'ReadWrite, Delete', 65536)
  $sr = [System.IO.StreamReader]::new($fs, (New-Object System.Text.UTF8Encoding($false)), $true, 65536)
  [void](Get-DiagJsonSerializer)
  return [pscustomobject]@{
    Path       = $Path
    Reader     = $sr
    Buffer     = (New-Object char[] $ChunkChars)
    Carry      = ''
    Eof        = $false
    Records    = [long]0
    BadLines   = [long]0
    Truncated  = $false
    SlowChunks = 0
  }
}

function Close-JsonlReader {
  param($Reader)
  if ($Reader -and $Reader.Reader) {
    try { $Reader.Reader.Dispose() } catch { }
  }
}

function ConvertFrom-JsonlChunk {
  param($Reader, [string]$Body)
  $json = '[' + $Body.Replace("`n", ',') + ']'
  try {
    $arr = ConvertFrom-DiagJsonArrayText -Text $json
    $Reader.Records += $arr.Length
    return , $arr
  } catch {
    $Reader.SlowChunks++
  }
  $list = New-Object System.Collections.Generic.List[object]
  foreach ($line in $Body.Split([char]10)) {
    $l = $line.Trim()
    if ($l.Length -eq 0) { continue }
    try {
      $o = ConvertFrom-DiagJsonLine -Text $l
      if ($null -ne $o) { $list.Add($o) }
    } catch {
      $Reader.BadLines++
    }
  }
  $Reader.Records += $list.Count
  return , $list.ToArray()
}

function Read-JsonlBatch {
  <#
    Returns the next batch of parsed records (object[]), an empty array when a
    chunk held no complete line, or $null at end of file. A final line without
    a newline is parsed if it is complete and otherwise counted as truncated
    (the process died while writing it).
  #>
  param([Parameter(Mandatory = $true)]$Reader)
  if ($Reader.Eof) { return $null }
  $n = $Reader.Reader.Read($Reader.Buffer, 0, $Reader.Buffer.Length)
  if ($n -le 0) {
    $Reader.Eof = $true
    $tail = $Reader.Carry.Trim()
    $Reader.Carry = ''
    if ($tail.Length -eq 0) { return $null }
    try {
      $o = ConvertFrom-DiagJsonLine -Text $tail
      $Reader.Records++
      return , @($o)
    } catch {
      $Reader.Truncated = $true
      $Reader.BadLines++
      return $null
    }
  }
  $text = $Reader.Carry + [string]::new($Reader.Buffer, 0, $n)
  $last = $text.LastIndexOf([char]10)
  if ($last -lt 0) {
    $Reader.Carry = $text
    return , @()
  }
  $Reader.Carry = $text.Substring($last + 1)
  $body = $text.Substring(0, $last).Trim()
  if ($body.Length -eq 0) { return , @() }
  return (ConvertFrom-JsonlChunk -Reader $Reader -Body $body)
}

# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

function Get-Percentile {
  <# Linear interpolation between closest ranks. $Sorted must be sorted ascending. #>
  param([double[]]$Sorted, [double]$P)
  if ($null -eq $Sorted -or $Sorted.Length -eq 0) { return $null }
  $n = $Sorted.Length
  if ($n -eq 1) { return $Sorted[0] }
  $rank = ($P / 100.0) * ($n - 1)
  $lo = [int][Math]::Floor($rank)
  $hi = [int][Math]::Ceiling($rank)
  return $Sorted[$lo] + ($Sorted[$hi] - $Sorted[$lo]) * ($rank - $lo)
}

function Get-SampleStats {
  <# Count, mean, min, max and percentiles of a list of numbers. #>
  param($Values)
  $arr = [double[]]@($Values)
  $n = $arr.Length
  if ($n -eq 0) {
    return [ordered]@{ n = 0; mean = $null; min = $null; max = $null; p50 = $null; p95 = $null; p99 = $null; sum = 0 }
  }
  [Array]::Sort($arr)
  $sum = 0.0
  foreach ($v in $arr) { $sum += $v }
  return [ordered]@{
    n    = $n
    mean = $sum / $n
    min  = $arr[0]
    max  = $arr[$n - 1]
    p50  = Get-Percentile -Sorted $arr -P 50
    p95  = Get-Percentile -Sorted $arr -P 95
    p99  = Get-Percentile -Sorted $arr -P 99
    sum  = $sum
  }
}

function Get-LinearFit {
  <# Least-squares line through (X, Y): slope, intercept and r-squared. #>
  param([double[]]$X, [double[]]$Y)
  $n = [Math]::Min($X.Length, $Y.Length)
  if ($n -lt 2) { return $null }
  $sx = 0.0; $sy = 0.0
  for ($i = 0; $i -lt $n; $i++) { $sx += $X[$i]; $sy += $Y[$i] }
  $mx = $sx / $n; $my = $sy / $n
  $sxx = 0.0; $sxy = 0.0; $syy = 0.0
  for ($i = 0; $i -lt $n; $i++) {
    $dx = $X[$i] - $mx; $dy = $Y[$i] - $my
    $sxx += $dx * $dx; $sxy += $dx * $dy; $syy += $dy * $dy
  }
  if ($sxx -eq 0) { return $null }
  $slope = $sxy / $sxx
  $r2 = 0.0
  if ($syy -gt 0) { $r2 = ($sxy * $sxy) / ($sxx * $syy) }
  return [ordered]@{ n = $n; slope = $slope; intercept = $my - $slope * $mx; r2 = $r2 }
}

# ---------------------------------------------------------------------------
# Human formatting. Always invariant culture: a Romanian Windows would otherwise
# write "12,5", which breaks SVG coordinates and reads as a list in tables.
# ---------------------------------------------------------------------------

function Format-DiagNumber {
  param($Value, [int]$Decimals = 1)
  if ($null -eq $Value -or ($Value -is [string] -and $Value -eq '')) { return '-' }
  $d = [double]$Value
  if ([double]::IsNaN($d) -or [double]::IsInfinity($d)) { return '-' }
  return $d.ToString('F' + $Decimals, $script:Inv)
}

function Format-DiagMs {
  <# 850 ms / 12.3 s / 4 min 05 s / 1 h 02 min #>
  param($Ms)
  if ($null -eq $Ms) { return '-' }
  $v = [double]$Ms
  if ($v -lt 10) { return $v.ToString('0.0', $script:Inv) + ' ms' }
  if ($v -lt 1000) { return $v.ToString('0', $script:Inv) + ' ms' }
  $s = $v / 1000.0
  if ($s -lt 60) { return $s.ToString('0.0', $script:Inv) + ' s' }
  if ($s -lt 3600) {
    $m = [Math]::Floor($s / 60)
    return $m.ToString('0', $script:Inv) + ' min ' + ([Math]::Floor($s - 60 * $m)).ToString('00', $script:Inv) + ' s'
  }
  $h = [Math]::Floor($s / 3600)
  return $h.ToString('0', $script:Inv) + ' h ' + ([Math]::Floor(($s - 3600 * $h) / 60)).ToString('00', $script:Inv) + ' min'
}

function Format-DiagBytes {
  param($Bytes)
  if ($null -eq $Bytes) { return '-' }
  $b = [double]$Bytes
  if ($b -lt 1024) { return $b.ToString('0', $script:Inv) + ' B' }
  if ($b -lt 1048576) { return ($b / 1024).ToString('0.0', $script:Inv) + ' KB' }
  if ($b -lt 1073741824) { return ($b / 1048576).ToString('0.0', $script:Inv) + ' MB' }
  return ($b / 1073741824).ToString('0.00', $script:Inv) + ' GB'
}

function Format-DiagMB {
  param($MB)
  if ($null -eq $MB) { return '-' }
  return Format-DiagBytes -Bytes ([double]$MB * 1048576)
}

function Format-DiagWhen {
  <#
    A timestamp from JSON for display. PowerShell 7's ConvertFrom-Json turns
    ISO strings into DateTime, 5.1 leaves them as strings; both end up alike.
  #>
  param($Value)
  if ($null -eq $Value) { return '-' }
  if ($Value -is [datetime]) { return $Value.ToString('yyyy-MM-dd HH:mm:ss', $script:Inv) }
  if ($Value -is [datetimeoffset]) { return $Value.ToString('yyyy-MM-dd HH:mm:ss', $script:Inv) }
  $s = [string]$Value
  $m = [regex]::Match($s, '^(\d{4}-\d{2}-\d{2})T(\d{2}:\d{2}:\d{2})')
  if ($m.Success) { return $m.Groups[1].Value + ' ' + $m.Groups[2].Value }
  return $s
}

function Format-DiagInvariant {
  <# Culture-independent number for SVG attributes. #>
  param([double]$Value)
  return $Value.ToString('0.##', $script:Inv)
}

# ---------------------------------------------------------------------------
# HTML and SVG
# ---------------------------------------------------------------------------

function ConvertTo-DiagHtml {
  param($Text)
  if ($null -eq $Text) { return '' }
  return [System.Net.WebUtility]::HtmlEncode([string]$Text)
}

function New-DiagHtmlTable {
  <#
    $Rows is a list of arrays of cells. A cell is plain text (encoded here) or
    a hashtable: @{ html = '<b>raw</b>' } and/or @{ text = '..'; cls = 'warn' }.
    $NumericColumns are right-aligned.
  #>
  param([string[]]$Headers, $Rows, [int[]]$NumericColumns = @(), [string]$Class = '', [string]$Caption = '')
  $sb = New-Object System.Text.StringBuilder
  [void]$sb.Append('<div class="tablewrap"><table')
  if ($Class) { [void]$sb.Append(' class="' + $Class + '"') }
  [void]$sb.Append('>')
  if ($Caption) { [void]$sb.Append('<caption>' + (ConvertTo-DiagHtml $Caption) + '</caption>') }
  [void]$sb.Append('<thead><tr>')
  for ($i = 0; $i -lt $Headers.Length; $i++) {
    $cls = ''
    if ($NumericColumns -contains $i) { $cls = ' class="num"' }
    [void]$sb.Append('<th' + $cls + '>' + (ConvertTo-DiagHtml $Headers[$i]) + '</th>')
  }
  [void]$sb.Append('</tr></thead><tbody>')
  foreach ($row in $Rows) {
    [void]$sb.Append('<tr>')
    for ($i = 0; $i -lt $row.Length; $i++) {
      $cell = $row[$i]
      $classes = @()
      if ($NumericColumns -contains $i) { $classes += 'num' }
      $content = ''
      if ($cell -is [System.Collections.IDictionary]) {
        if ($cell.Contains('cls') -and $cell['cls']) { $classes += [string]$cell['cls'] }
        if ($cell.Contains('html')) { $content = [string]$cell['html'] }
        else { $content = ConvertTo-DiagHtml $cell['text'] }
      } else {
        $content = ConvertTo-DiagHtml $cell
      }
      if ($classes.Count -gt 0) {
        [void]$sb.Append('<td class="' + ($classes -join ' ') + '">' + $content + '</td>')
      } else {
        [void]$sb.Append('<td>' + $content + '</td>')
      }
    }
    [void]$sb.Append('</tr>')
  }
  [void]$sb.Append('</tbody></table></div>')
  return $sb.ToString()
}

function Get-DiagNiceTicks {
  param([double]$Min, [double]$Max, [int]$Count = 5)
  if ($Max -le $Min) { $Max = $Min + 1 }
  $range = $Max - $Min
  $rough = $range / $Count
  $mag = [Math]::Pow(10, [Math]::Floor([Math]::Log10($rough)))
  $norm = $rough / $mag
  $step = 10
  if ($norm -lt 1.5) { $step = 1 } elseif ($norm -lt 3) { $step = 2 } elseif ($norm -lt 7) { $step = 5 }
  $step = $step * $mag
  $lo = [Math]::Floor($Min / $step) * $step
  $hi = [Math]::Ceiling($Max / $step) * $step
  if ($hi -le $lo) { $hi = $lo + $step }
  $ticks = New-Object System.Collections.Generic.List[double]
  $v = $lo
  $guard = 0
  while ($v -le $hi + $step * 0.001 -and $guard -lt 50) {
    $ticks.Add([Math]::Round($v, 10))
    $v += $step
    $guard++
  }
  $dec = 0
  if ($step -lt 1) { $dec = [int][Math]::Ceiling(-[Math]::Log10($step)) }
  return @{ Min = $lo; Max = $hi; Step = $step; Ticks = $ticks.ToArray(); Decimals = $dec }
}

function Format-DiagTick {
  param([double]$Value, [int]$Decimals)
  $a = [Math]::Abs($Value)
  if ($a -ge 1000000 -and $Decimals -eq 0) { return ($Value / 1000000).ToString('0.#', $script:Inv) + 'M' }
  if ($a -ge 10000 -and $Decimals -eq 0) { return ($Value / 1000).ToString('0.#', $script:Inv) + 'k' }
  return $Value.ToString('F' + $Decimals, $script:Inv)
}

function Get-DiagDownsampled {
  <#
    Reduces a series to at most $MaxPoints by bucketing. Keeping the bucket
    maximum preserves peaks and leak trends; 'mean' suits rates like CPU.
  #>
  param([double[]]$X, [double[]]$Y, [int]$MaxPoints = 1000, [string]$Mode = 'max')
  $n = $X.Length
  if ($n -le $MaxPoints) { return @{ X = $X; Y = $Y } }
  $ox = New-Object double[] $MaxPoints
  $oy = New-Object double[] $MaxPoints
  for ($b = 0; $b -lt $MaxPoints; $b++) {
    $s = [int][Math]::Floor($b * $n / $MaxPoints)
    $e = [int][Math]::Floor(($b + 1) * $n / $MaxPoints) - 1
    if ($e -lt $s) { $e = $s }
    $acc = $Y[$s]
    if ($Mode -eq 'mean') {
      $sum = 0.0
      for ($i = $s; $i -le $e; $i++) { $sum += $Y[$i] }
      $acc = $sum / ($e - $s + 1)
    } else {
      for ($i = $s + 1; $i -le $e; $i++) { if ($Y[$i] -gt $acc) { $acc = $Y[$i] } }
    }
    $ox[$b] = $X[$e]
    $oy[$b] = $acc
  }
  return @{ X = $ox; Y = $oy }
}

function New-DiagSvgLineChart {
  <#
    $Series: array of hashtables @{ Name; Color; X = double[]; Y = double[] }.
    $VLines: array of @{ X; Color; Label } drawn as dashed markers (cycle starts).
    $Marks:  array of @{ X; Y; Color; Title } drawn as dots with a tooltip.
  #>
  param(
    [object[]]$Series,
    [string]$Title = '',
    [string]$YLabel = '',
    [string]$XLabel = 'minutes since start',
    [int]$Width = 920,
    [int]$Height = 230,
    [object[]]$VLines = @(),
    [object[]]$Marks = @(),
    [string]$Mode = 'max',
    [int]$MaxPoints = 900,
    [object[]]$Legend = @(),
    [switch]$ZeroBased
  )
  $ml = 58; $mr = 14; $mt = 10; $mb = 32
  $pw = $Width - $ml - $mr
  $ph = $Height - $mt - $mb
  $xmin = [double]::MaxValue; $xmax = [double]::MinValue
  $ymin = [double]::MaxValue; $ymax = [double]::MinValue
  $prepared = @()
  foreach ($s in $Series) {
    if ($null -eq $s.X -or $s.X.Length -eq 0) { continue }
    $d = Get-DiagDownsampled -X ([double[]]$s.X) -Y ([double[]]$s.Y) -MaxPoints $MaxPoints -Mode $Mode
    foreach ($v in $d.X) { if ($v -lt $xmin) { $xmin = $v }; if ($v -gt $xmax) { $xmax = $v } }
    foreach ($v in $d.Y) { if ($v -lt $ymin) { $ymin = $v }; if ($v -gt $ymax) { $ymax = $v } }
    $prepared += , @{ Name = $s.Name; Color = $s.Color; X = $d.X; Y = $d.Y }
  }
  foreach ($m in $Marks) {
    $mx = [double]$m.X; $my = [double]$m.Y
    if ($mx -lt $xmin) { $xmin = $mx }; if ($mx -gt $xmax) { $xmax = $mx }
    if ($my -lt $ymin) { $ymin = $my }; if ($my -gt $ymax) { $ymax = $my }
  }
  if ($prepared.Count -eq 0 -and $Marks.Count -eq 0) {
    return '<p class="muted">No data for this chart.</p>'
  }
  if ($xmax -le $xmin) { $xmax = $xmin + 1 }
  if ($ZeroBased -or ($ymin -ge 0 -and $ymin -lt 0.35 * $ymax)) { $ymin = [Math]::Min(0, $ymin) }
  if ($ymax -le $ymin) { $ymax = $ymin + 1 }
  $yt = Get-DiagNiceTicks -Min $ymin -Max $ymax -Count 4
  $xt = Get-DiagNiceTicks -Min $xmin -Max $xmax -Count 8
  $x0 = $xt.Min; $x1 = $xt.Max; $y0 = $yt.Min; $y1 = $yt.Max
  $sxf = $pw / ($x1 - $x0)
  $syf = $ph / ($y1 - $y0)

  $sb = New-Object System.Text.StringBuilder
  [void]$sb.Append('<figure class="chart">')
  if ($Title) { [void]$sb.Append('<figcaption>' + (ConvertTo-DiagHtml $Title) + '</figcaption>') }
  $leg = $Legend
  if ($leg.Count -eq 0 -and $prepared.Count -gt 1) { $leg = $prepared }
  if ($leg.Count -gt 0) {
    [void]$sb.Append('<div class="legend">')
    foreach ($p in $leg) {
      [void]$sb.Append('<span><i style="background:' + $p.Color + '"></i>' + (ConvertTo-DiagHtml $p.Name) + '</span>')
    }
    [void]$sb.Append('</div>')
  }
  [void]$sb.Append('<svg viewBox="0 0 ' + $Width + ' ' + $Height + '" width="100%" role="img" preserveAspectRatio="xMinYMin meet"')
  [void]$sb.Append(' aria-label="' + (ConvertTo-DiagHtml $Title) + '">')
  foreach ($t in $yt.Ticks) {
    $py = $mt + $ph - ($t - $y0) * $syf
    [void]$sb.Append('<line class="grid" x1="' + $ml + '" x2="' + ($ml + $pw) + '" y1="' + (Format-DiagInvariant $py) + '" y2="' + (Format-DiagInvariant $py) + '"/>')
    [void]$sb.Append('<text class="tick" x="' + ($ml - 6) + '" y="' + (Format-DiagInvariant ($py + 4)) + '" text-anchor="end">' + (Format-DiagTick -Value $t -Decimals $yt.Decimals) + '</text>')
  }
  foreach ($t in $xt.Ticks) {
    $px = $ml + ($t - $x0) * $sxf
    [void]$sb.Append('<line class="axis" x1="' + (Format-DiagInvariant $px) + '" x2="' + (Format-DiagInvariant $px) + '" y1="' + ($mt + $ph) + '" y2="' + ($mt + $ph + 4) + '"/>')
    [void]$sb.Append('<text class="tick" x="' + (Format-DiagInvariant $px) + '" y="' + ($mt + $ph + 16) + '" text-anchor="middle">' + (Format-DiagTick -Value $t -Decimals $xt.Decimals) + '</text>')
  }
  [void]$sb.Append('<line class="axis" x1="' + $ml + '" x2="' + ($ml + $pw) + '" y1="' + ($mt + $ph) + '" y2="' + ($mt + $ph) + '"/>')
  if ($YLabel) {
    [void]$sb.Append('<text class="axlabel" transform="translate(12,' + ($mt + $ph / 2) + ') rotate(-90)" text-anchor="middle">' + (ConvertTo-DiagHtml $YLabel) + '</text>')
  }
  if ($XLabel) {
    [void]$sb.Append('<text class="axlabel" x="' + ($ml + $pw) + '" y="' + ($Height - 2) + '" text-anchor="end">' + (ConvertTo-DiagHtml $XLabel) + '</text>')
  }
  foreach ($v in $VLines) {
    $px = $ml + ([double]$v.X - $x0) * $sxf
    $col = '#8a8984'
    if ($v.Color) { $col = $v.Color }
    [void]$sb.Append('<line class="vline" stroke="' + $col + '" x1="' + (Format-DiagInvariant $px) + '" x2="' + (Format-DiagInvariant $px) + '" y1="' + $mt + '" y2="' + ($mt + $ph) + '">')
    if ($v.Label) { [void]$sb.Append('<title>' + (ConvertTo-DiagHtml $v.Label) + '</title>') }
    [void]$sb.Append('</line>')
  }
  foreach ($p in $prepared) {
    $path = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $p.X.Length; $i++) {
      $px = $ml + ($p.X[$i] - $x0) * $sxf
      $py = $mt + $ph - ($p.Y[$i] - $y0) * $syf
      if ($i -eq 0) { [void]$path.Append('M') } else { [void]$path.Append('L') }
      [void]$path.Append($px.ToString('0.#', $script:Inv) + ' ' + $py.ToString('0.#', $script:Inv))
    }
    [void]$sb.Append('<path class="series" stroke="' + $p.Color + '" d="' + $path.ToString() + '"><title>' + (ConvertTo-DiagHtml $p.Name) + '</title></path>')
  }
  foreach ($m in $Marks) {
    $px = $ml + ([double]$m.X - $x0) * $sxf
    $py = $mt + $ph - ([double]$m.Y - $y0) * $syf
    $col = '#2a78d6'
    if ($m.Color) { $col = $m.Color }
    [void]$sb.Append('<circle class="mark" cx="' + (Format-DiagInvariant $px) + '" cy="' + (Format-DiagInvariant $py) + '" r="4" fill="' + $col + '">')
    if ($m.Title) { [void]$sb.Append('<title>' + (ConvertTo-DiagHtml $m.Title) + '</title>') }
    [void]$sb.Append('</circle>')
  }
  [void]$sb.Append('</svg></figure>')
  return $sb.ToString()
}

function New-DiagSvgBarChart {
  <# Vertical bars with the value above each; $Titles become hover tooltips. #>
  param(
    [string[]]$Labels,
    [double[]]$Values,
    [string]$Title = '',
    [string]$Color = '#2a78d6',
    [string[]]$Colors = @(),
    [string[]]$ValueLabels = @(),
    [string[]]$Titles = @(),
    [string]$YLabel = '',
    [int]$Width = 620,
    [int]$Height = 220
  )
  if ($null -eq $Values -or $Values.Length -eq 0) { return '<p class="muted">No data for this chart.</p>' }
  $ml = 50; $mr = 10; $mt = 18; $mb = 40
  $pw = $Width - $ml - $mr
  $ph = $Height - $mt - $mb
  $max = 0.0
  foreach ($v in $Values) { if ($v -gt $max) { $max = $v } }
  if ($max -le 0) { $max = 1 }
  $yt = Get-DiagNiceTicks -Min 0 -Max $max -Count 4
  $syf = $ph / ($yt.Max - $yt.Min)
  $n = $Values.Length
  $slot = $pw / $n
  $bw = [Math]::Max(4, [Math]::Min(56, $slot * 0.7))
  $sb = New-Object System.Text.StringBuilder
  [void]$sb.Append('<figure class="chart small">')
  if ($Title) { [void]$sb.Append('<figcaption>' + (ConvertTo-DiagHtml $Title) + '</figcaption>') }
  [void]$sb.Append('<svg viewBox="0 0 ' + $Width + ' ' + $Height + '" width="100%" role="img" aria-label="' + (ConvertTo-DiagHtml $Title) + '">')
  foreach ($t in $yt.Ticks) {
    $py = $mt + $ph - ($t - $yt.Min) * $syf
    [void]$sb.Append('<line class="grid" x1="' + $ml + '" x2="' + ($ml + $pw) + '" y1="' + (Format-DiagInvariant $py) + '" y2="' + (Format-DiagInvariant $py) + '"/>')
    [void]$sb.Append('<text class="tick" x="' + ($ml - 6) + '" y="' + (Format-DiagInvariant ($py + 4)) + '" text-anchor="end">' + (Format-DiagTick -Value $t -Decimals $yt.Decimals) + '</text>')
  }
  $base = $mt + $ph
  for ($i = 0; $i -lt $n; $i++) {
    $v = [double]$Values[$i]
    $h = $v * $syf
    $x = $ml + $slot * $i + ($slot - $bw) / 2
    $y = $base - $h
    $col = $Color
    if ($Colors.Length -gt $i -and $Colors[$i]) { $col = $Colors[$i] }
    $r = [Math]::Min(4, [Math]::Min($bw / 2, $h))
    if ($h -gt 0.5) {
      $d = 'M' + (Format-DiagInvariant $x) + ' ' + (Format-DiagInvariant $base) +
      ' V' + (Format-DiagInvariant ($y + $r)) +
      ' Q' + (Format-DiagInvariant $x) + ' ' + (Format-DiagInvariant $y) + ' ' + (Format-DiagInvariant ($x + $r)) + ' ' + (Format-DiagInvariant $y) +
      ' H' + (Format-DiagInvariant ($x + $bw - $r)) +
      ' Q' + (Format-DiagInvariant ($x + $bw)) + ' ' + (Format-DiagInvariant $y) + ' ' + (Format-DiagInvariant ($x + $bw)) + ' ' + (Format-DiagInvariant ($y + $r)) +
      ' V' + (Format-DiagInvariant $base) + ' Z'
      [void]$sb.Append('<path fill="' + $col + '" d="' + $d + '">')
      if ($Titles.Length -gt $i) { [void]$sb.Append('<title>' + (ConvertTo-DiagHtml $Titles[$i]) + '</title>') }
      [void]$sb.Append('</path>')
    }
    $vl = ''
    if ($ValueLabels.Length -gt $i) { $vl = $ValueLabels[$i] } elseif ($v -ne 0) { $vl = Format-DiagTick -Value $v -Decimals 0 }
    if ($vl) {
      [void]$sb.Append('<text class="vlabel" x="' + (Format-DiagInvariant ($x + $bw / 2)) + '" y="' + (Format-DiagInvariant ($y - 4)) + '" text-anchor="middle">' + (ConvertTo-DiagHtml $vl) + '</text>')
    }
    [void]$sb.Append('<text class="tick" x="' + (Format-DiagInvariant ($x + $bw / 2)) + '" y="' + ($base + 15) + '" text-anchor="middle">' + (ConvertTo-DiagHtml $Labels[$i]) + '</text>')
  }
  [void]$sb.Append('<line class="axis" x1="' + $ml + '" x2="' + ($ml + $pw) + '" y1="' + $base + '" y2="' + $base + '"/>')
  if ($YLabel) {
    [void]$sb.Append('<text class="axlabel" transform="translate(12,' + ($mt + $ph / 2) + ') rotate(-90)" text-anchor="middle">' + (ConvertTo-DiagHtml $YLabel) + '</text>')
  }
  [void]$sb.Append('</svg></figure>')
  return $sb.ToString()
}

function New-DiagInlineBar {
  <# A proportional bar inside a table cell. #>
  param([double]$Value, [double]$Max, [string]$Color = '#2a78d6')
  if ($Max -le 0) { return '' }
  $pct = [Math]::Max(0.5, [Math]::Min(100, 100.0 * $Value / $Max))
  return '<span class="ibar"><span style="width:' + $pct.ToString('0.#', $script:Inv) + '%;background:' + $Color + '"></span></span>'
}

function Get-DiagStatusBadge {
  param([string]$Status)
  $cls = 'na'
  switch ($Status) {
    'PASS' { $cls = 'pass' }
    'WARN' { $cls = 'warn' }
    'FAIL' { $cls = 'fail' }
    default { $cls = 'na' }
  }
  $icon = @{ pass = '&#10003;'; warn = '!'; fail = '&#10007;'; na = '&#8211;' }[$cls]
  return '<span class="badge ' + $cls + '"><b>' + $icon + '</b>' + (ConvertTo-DiagHtml $Status) + '</span>'
}

function Get-DiagReportCss {
  return @'
:root{--surface:#fcfcfb;--panel:#ffffff;--ink:#0b0b0b;--ink2:#52514e;--muted:#8a8984;--line:#e6e5e0;
--good:#0ca30c;--warning:#fab219;--serious:#ec835a;--critical:#d03b3b;--accent:#2a78d6;color-scheme:light}
*{box-sizing:border-box}
body{margin:0;background:var(--surface);color:var(--ink);font:14px/1.45 "Segoe UI",system-ui,-apple-system,Arial,sans-serif}
main{max-width:1180px;margin:0 auto;padding:20px 16px 60px}
h1{font-size:24px;margin:8px 0 2px}
h2{font-size:19px;margin:34px 0 10px;padding-top:8px;border-top:1px solid var(--line)}
h3{font-size:15px;margin:20px 0 8px}
p{margin:6px 0}
.muted{color:var(--muted)}
.sub{color:var(--ink2);margin-bottom:14px}
nav.toc{display:flex;flex-wrap:wrap;gap:6px 14px;margin:12px 0 4px;font-size:13px}
nav.toc a{color:var(--accent);text-decoration:none}
nav.toc a:hover{text-decoration:underline}
.tablewrap{overflow-x:auto;margin:6px 0 12px}
table{border-collapse:collapse;min-width:40%;background:var(--panel);font-size:13px}
caption{text-align:left;color:var(--ink2);padding:4px 0;font-weight:600}
th,td{border-bottom:1px solid var(--line);padding:4px 10px;text-align:left;vertical-align:top}
th{background:#f3f2ee;font-weight:600;color:var(--ink2);white-space:nowrap}
tbody tr:nth-child(even){background:#fafaf8}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
td.mono,.mono{font-family:Consolas,"Cascadia Mono",monospace;font-size:12px}
td.hl{background:#fdecd9 !important;font-weight:600}
td.bad{background:#f9dcdc !important;font-weight:600}
td.good{background:#e3f4e3 !important}
td.diff{background:#fff4d6 !important}
.badge{display:inline-flex;align-items:center;gap:4px;padding:1px 8px;border-radius:10px;font-size:12px;font-weight:700;white-space:nowrap;border:1px solid transparent}
.badge b{font-weight:700}
.badge.pass{background:#e3f4e3;color:#075d07;border-color:#0ca30c}
.badge.warn{background:#fff4d6;color:#7a5400;border-color:#fab219}
.badge.fail{background:#f9dcdc;color:#8f1f1f;border-color:#d03b3b}
.badge.na{background:#f0efec;color:#52514e;border-color:#c9c8c2}
.verdict{border:1px solid var(--line);border-radius:10px;background:var(--panel);padding:14px 16px;margin:14px 0}
.verdict .overall{font-size:17px;font-weight:600;margin-bottom:8px;display:flex;gap:10px;align-items:center}
.verdict table{width:100%}
.note{background:#f3f2ee;border-left:3px solid #c9c8c2;padding:8px 12px;margin:10px 0;color:var(--ink2);font-size:13px}
.hint{background:#fff9ea;border-left:3px solid var(--warning);padding:6px 12px;margin:6px 0;font-size:13px}
figure.chart{margin:10px 0 18px;background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:8px 10px 4px}
figure.chart.small{max-width:640px;display:inline-block;margin-right:12px;vertical-align:top;width:100%}
figcaption{font-weight:600;color:var(--ink2);font-size:13px;margin-bottom:2px}
.legend{display:flex;flex-wrap:wrap;gap:4px 14px;font-size:12px;color:var(--ink2);margin:2px 0 4px}
.legend i{display:inline-block;width:14px;height:3px;border-radius:2px;margin-right:5px;vertical-align:middle}
svg text{font-family:"Segoe UI",system-ui,Arial,sans-serif}
svg .grid{stroke:#e6e5e0;stroke-width:1}
svg .axis{stroke:#b5b4ae;stroke-width:1}
svg .tick{font-size:11px;fill:#6b6a66}
svg .axlabel{font-size:11px;fill:#52514e}
svg .vlabel{font-size:11px;fill:#0b0b0b}
svg .series{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
svg .vline{stroke-width:1;stroke-dasharray:3 3;opacity:.55}
svg .mark{stroke:#fff;stroke-width:2}
.ibar{display:inline-block;width:120px;height:8px;background:#f0efec;border-radius:4px;vertical-align:middle;margin-left:6px}
.ibar span{display:block;height:8px;border-radius:4px}
details{margin:6px 0}
summary{cursor:pointer;color:var(--accent)}
pre{background:#f6f6f3;border:1px solid var(--line);padding:8px;overflow-x:auto;font-size:12px;max-height:420px}
.kv{display:grid;grid-template-columns:max-content 1fr;gap:2px 16px;font-size:13px}
.kv div:nth-child(odd){color:var(--ink2)}
table.cmp{table-layout:fixed;width:100%}
table.cmp th,table.cmp td{overflow-wrap:anywhere;white-space:normal}
table.cmp th:first-child,table.cmp td:first-child{width:24%}
.grid2{display:grid;grid-template-columns:repeat(auto-fit,minmax(420px,1fr));gap:12px}
@media (max-width:600px){.grid2{grid-template-columns:1fr} .ibar{width:60px}}
'@
}

# ---------------------------------------------------------------------------
# Machine inventory
# ---------------------------------------------------------------------------

function Get-DirectoryStats {
  <#
    Counts files and bytes under $Path, bounded in entries and time so a huge
    %TEMP% or a slow share cannot hang the run. Complete = $false means "at least".
  #>
  param([string]$Path, [int]$MaxEntries = 200000, [double]$MaxSeconds = 20, [switch]$Recurse, [string]$ImagePattern = '')
  $r = [ordered]@{ path = $Path; exists = $false; files = 0; dirs = 0; bytes = [long]0; images = 0; complete = $true; seconds = 0.0; errors = 0 }
  if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Container)) { return $r }
  $r.exists = $true
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $stack = New-Object System.Collections.Generic.Stack[string]
  $stack.Push($Path)
  $entries = 0
  $imgRx = $null
  if ($ImagePattern) { $imgRx = New-Object System.Text.RegularExpressions.Regex($ImagePattern, 'IgnoreCase') }
  :outer while ($stack.Count -gt 0) {
    $dir = $stack.Pop()
    try {
      $di = [System.IO.DirectoryInfo]::new($dir)
      foreach ($e in $di.EnumerateFileSystemInfos()) {
        $entries++
        if (($entries % 500) -eq 0) {
          if ($entries -ge $MaxEntries -or $sw.Elapsed.TotalSeconds -ge $MaxSeconds) { $r.complete = $false; break outer }
        }
        if ($e -is [System.IO.DirectoryInfo]) {
          $r.dirs++
          if ($Recurse -and -not ($e.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) { $stack.Push($e.FullName) }
        } else {
          $r.files++
          $r.bytes += $e.Length
          if ($imgRx -and $imgRx.IsMatch($e.Name)) { $r.images++ }
        }
      }
    } catch {
      $r.errors++
    }
    if ($entries -ge $MaxEntries -or $sw.Elapsed.TotalSeconds -ge $MaxSeconds) {
      if ($stack.Count -gt 0) { $r.complete = $false }
      break
    }
  }
  $r.seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 2)
  return $r
}

function Invoke-DiagSafe {
  <# Runs one inventory probe; a failure is recorded, never thrown. #>
  param([scriptblock]$Block, [string]$What, $Errors)
  try {
    # The comma keeps a one-element list a list for the caller.
    $r = & $Block
    return , $r
  } catch {
    if ($null -ne $Errors) { $Errors.Add($What + ': ' + $_.Exception.Message) }
    return $null
  }
}

function Get-DiagCim {
  param([string]$Class, [string]$Namespace = 'root/cimv2', [string]$Filter = '')
  if ($Filter) {
    return @(Get-CimInstance -Namespace $Namespace -ClassName $Class -Filter $Filter -OperationTimeoutSec 20 -ErrorAction Stop)
  }
  return @(Get-CimInstance -Namespace $Namespace -ClassName $Class -OperationTimeoutSec 20 -ErrorAction Stop)
}

function ConvertTo-DiagIso {
  param($Date)
  if ($null -eq $Date) { return $null }
  try { return ([datetime]$Date).ToString('yyyy-MM-ddTHH:mm:ss', $script:Inv) } catch { return [string]$Date }
}

$script:ProcessesOfInterest = @{
  'onedrive' = 'sync'; 'dropbox' = 'sync'; 'googledrivefs' = 'sync'; 'googledrivesync' = 'sync'; 'box' = 'sync'
  'nextcloud' = 'sync'; 'owncloud' = 'sync'; 'syncthing' = 'sync'; 'icloudservices' = 'sync'; 'icloud' = 'sync'
  'searchindexer' = 'indexer'; 'searchprotocolhost' = 'indexer'; 'searchfilterhost' = 'indexer'
  'msmpeng' = 'antivirus'; 'mpdefendercoreservice' = 'antivirus'; 'nissrv' = 'antivirus'
  'mssense' = 'edr'; 'sensecncproxy' = 'edr'; 'senseir' = 'edr'; 'sensendr' = 'edr'
  'csfalconservice' = 'edr'; 'csfalconcontainer' = 'edr'; 'sentinelagent' = 'edr'; 'sentinelservicehost' = 'edr'
  'cylancesvc' = 'edr'; 'cb' = 'edr'; 'repmgr' = 'edr'; 'taniumclient' = 'edr'; 'cyserver' = 'edr'; 'cytray' = 'edr'
  'elastic-agent' = 'edr'; 'elastic-endpoint' = 'edr'; 'xagt' = 'edr'; 'ekrn' = 'antivirus'; 'egui' = 'antivirus'
  'avp' = 'antivirus'; 'avpui' = 'antivirus'; 'bdagent' = 'antivirus'; 'vsserv' = 'antivirus'; 'bdservicehost' = 'antivirus'
  'epsecurityservice' = 'antivirus'; 'sophosfilescanner' = 'antivirus'; 'savservice' = 'antivirus'; 'sophoshealth' = 'antivirus'
  'sedservice' = 'antivirus'; 'mcshield' = 'antivirus'; 'mfemms' = 'antivirus'; 'mfetp' = 'antivirus'; 'mfeesp' = 'antivirus'
  'ccsvchst' = 'antivirus'; 'nortonsecurity' = 'antivirus'; 'avastsvc' = 'antivirus'; 'aswengsrv' = 'antivirus'
  'avgsvc' = 'antivirus'; 'wrsa' = 'antivirus'; 'ntrtscan' = 'antivirus'; 'pccntmon' = 'antivirus'; 'tmccsf' = 'antivirus'
  'tmlisten' = 'antivirus'; 'fsav32' = 'antivirus'; 'fshoster32' = 'antivirus'; 'mbamservice' = 'antivirus'
  'veeam.endpoint.service' = 'backup'; 'veeamagent' = 'backup'; 'crashplanservice' = 'backup'; 'bzserv' = 'backup'
  'cobian' = 'backup'; 'acronis_monitor' = 'backup'; 'mms' = 'backup'; 'trueimagemonitor' = 'backup'; 'carbonite' = 'backup'
}

function Get-SystemInventory {
  <#
    Describes the machine as it matters for Scantailor-DGI performance. $Paths
    are the directories of interest (work dir, scans, shares): each gets its
    volume, file system, free space, network/local and a bounded file count.
  #>
  param([string[]]$Paths = @(), [switch]$SkipTempScan, [double]$TempScanSeconds = 20, [int]$TempScanMaxEntries = 200000)
  $errors = New-Object System.Collections.Generic.List[string]
  $inv = [ordered]@{}
  $inv.collected = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz', $script:Inv)
  $inv.computer = Get-DiagComputerName
  $inv.user = [System.Environment]::UserName
  $inv.powershell = $PSVersionTable.PSVersion.ToString() + ' ' + [string]$PSVersionTable.PSEdition
  $inv.culture = [System.Globalization.CultureInfo]::CurrentCulture.Name
  $inv.timezone = Invoke-DiagSafe -What 'timezone' -Errors $errors -Block { [System.TimeZoneInfo]::Local.Id }
  $inv.is_admin = Invoke-DiagSafe -What 'admin' -Errors $errors -Block {
    $p = New-Object System.Security.Principal.WindowsPrincipal([System.Security.Principal.WindowsIdentity]::GetCurrent())
    $p.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
  }
  if (-not (Test-DiagIsWindows)) {
    $inv.note = 'Not Windows: most probes skipped.'
    $inv.errors = @($errors)
    return $inv
  }

  $inv.os = Invoke-DiagSafe -What 'os' -Errors $errors -Block {
    $os = (Get-DiagCim Win32_OperatingSystem)[0]
    $cv = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
    $boot = [datetime]$os.LastBootUpTime
    [ordered]@{
      caption         = [string]$os.Caption
      version         = [string]$os.Version
      build           = [string]$os.BuildNumber
      ubr             = $cv.UBR
      display_version = [string]$cv.DisplayVersion
      edition         = [string]$cv.EditionID
      arch            = [string]$os.OSArchitecture
      last_boot       = ConvertTo-DiagIso $boot
      uptime_hours    = [Math]::Round(((Get-Date) - $boot).TotalHours, 1)
      free_ram_mb     = [Math]::Round([double]$os.FreePhysicalMemory / 1024)
      total_ram_mb    = [Math]::Round([double]$os.TotalVisibleMemorySize / 1024)
      commit_limit_mb = [Math]::Round([double]$os.TotalVirtualMemorySize / 1024)
      commit_free_mb  = [Math]::Round([double]$os.FreeVirtualMemory / 1024)
    }
  }
  $inv.system = Invoke-DiagSafe -What 'computer system' -Errors $errors -Block {
    $cs = (Get-DiagCim Win32_ComputerSystem)[0]
    [ordered]@{
      manufacturer       = [string]$cs.Manufacturer
      model              = [string]$cs.Model
      ram_mb             = [Math]::Round([double]$cs.TotalPhysicalMemory / 1048576)
      domain             = [string]$cs.Domain
      part_of_domain     = [bool]$cs.PartOfDomain
      auto_pagefile      = [bool]$cs.AutomaticManagedPagefile
      hypervisor_present = [bool]$cs.HypervisorPresent
    }
  }
  $inv.cpu = Invoke-DiagSafe -What 'cpu' -Errors $errors -Block {
    $list = @()
    foreach ($c in (Get-DiagCim Win32_Processor)) {
      $list += [ordered]@{
        name          = ([string]$c.Name).Trim()
        cores         = [int]$c.NumberOfCores
        logical       = [int]$c.NumberOfLogicalProcessors
        max_mhz       = [int]$c.MaxClockSpeed
        current_mhz   = [int]$c.CurrentClockSpeed
        load_pct      = $c.LoadPercentage
      }
    }
    , $list
  }
  $inv.physical_disks = Invoke-DiagSafe -What 'physical disks' -Errors $errors -Block {
    $list = @()
    if (Get-Command Get-PhysicalDisk -ErrorAction SilentlyContinue) {
      foreach ($d in @(Get-PhysicalDisk -ErrorAction Stop)) {
        $list += [ordered]@{
          number     = [string]$d.DeviceId
          name       = [string]$d.FriendlyName
          media_type = [string]$d.MediaType
          bus_type   = [string]$d.BusType
          size_gb    = [Math]::Round([double]$d.Size / 1GB, 1)
          health     = [string]$d.HealthStatus
          spindle    = $d.SpindleSpeed
        }
      }
    } else {
      foreach ($d in (Get-DiagCim Win32_DiskDrive)) {
        $list += [ordered]@{
          number     = [string]$d.Index
          name       = [string]$d.Model
          media_type = [string]$d.MediaType
          bus_type   = [string]$d.InterfaceType
          size_gb    = [Math]::Round([double]$d.Size / 1GB, 1)
        }
      }
    }
    , $list
  }
  $logical = Invoke-DiagSafe -What 'logical disks' -Errors $errors -Block { Get-DiagCim Win32_LogicalDisk }
  $inv.volumes = @()
  foreach ($ld in @($logical)) {
    if ($null -eq $ld) { continue }
    $inv.volumes += [ordered]@{
      drive      = [string]$ld.DeviceID
      type       = @{ 2 = 'removable'; 3 = 'local'; 4 = 'network'; 5 = 'cd'; 6 = 'ramdisk' }[[int]$ld.DriveType]
      fs         = [string]$ld.FileSystem
      size_gb    = [Math]::Round([double]$ld.Size / 1GB, 1)
      free_gb    = [Math]::Round([double]$ld.FreeSpace / 1GB, 1)
      provider   = [string]$ld.ProviderName
      label      = [string]$ld.VolumeName
    }
  }
  $inv.paths = @()
  foreach ($p in @($Paths)) {
    if (-not $p) { continue }
    $inv.paths += (Get-DiagPathInfo -Path $p -LogicalDisks @($logical) -PhysicalDisks @($inv.physical_disks) -Errors $errors)
  }
  # Where the user profile lives. Scantailor writes its settings INI (and,
  # installed, its logs and recovery files) there, and every settings panel
  # forces the INI to disk separately. On a profile redirected to a server each
  # of those writes is a network round trip on the GUI thread.
  $inv.profile = Invoke-DiagSafe -What 'profile location' -Errors $errors -Block {
    $roaming = [Environment]::GetFolderPath('ApplicationData')
    $local = [Environment]::GetFolderPath('LocalApplicationData')
    [ordered]@{
      appdata          = (Get-DiagPathInfo -Path $roaming -LogicalDisks @($logical) -PhysicalDisks @($inv.physical_disks) -Errors $errors)
      localappdata     = (Get-DiagPathInfo -Path $local -LogicalDisks @($logical) -PhysicalDisks @($inv.physical_disks) -Errors $errors)
      installed_ini    = (Join-Path $roaming 'scantailor-dgi\scantailor-dgi.ini')
      installed_ini_kb = $(if (Test-Path -LiteralPath (Join-Path $roaming 'scantailor-dgi\scantailor-dgi.ini')) { [Math]::Round((Get-Item -LiteralPath (Join-Path $roaming 'scantailor-dgi\scantailor-dgi.ini')).Length / 1KB, 1) } else { $null })
    }
  }
  # The first text Qt draws makes it walk every installed font (measured at
  # 1.2-2 s on a development PC), so the count differs PC to PC in a way that
  # matters.
  $inv.fonts = Invoke-DiagSafe -What 'installed fonts' -Errors $errors -Block {
    $count = 0
    $userCount = 0
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts'
    if (Test-Path $key) { $count = @((Get-Item $key).GetValueNames()).Count }
    $userKey = 'HKCU:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts'
    if (Test-Path $userKey) { $userCount = @((Get-Item $userKey).GetValueNames()).Count }
    $dirFiles = @(Get-ChildItem -LiteralPath (Join-Path $env:WINDIR 'Fonts') -File -ErrorAction SilentlyContinue).Count
    [ordered]@{ registered_machine = $count; registered_user = $userCount; files_in_windows_fonts = $dirFiles }
  }
  $inv.network_adapters = Invoke-DiagSafe -What 'network adapters' -Errors $errors -Block {
    $list = @()
    if (Get-Command Get-NetAdapter -ErrorAction SilentlyContinue) {
      foreach ($a in @(Get-NetAdapter -ErrorAction Stop | Where-Object { $_.Status -eq 'Up' })) {
        $list += [ordered]@{ name = [string]$a.Name; description = [string]$a.InterfaceDescription; link_speed = [string]$a.LinkSpeed; media = [string]$a.MediaType }
      }
    } else {
      foreach ($a in (Get-DiagCim Win32_NetworkAdapter -Filter 'NetEnabled = TRUE')) {
        $list += [ordered]@{ name = [string]$a.NetConnectionID; description = [string]$a.Name; link_speed = ([string]([Math]::Round([double]$a.Speed / 1e6)) + ' Mbps') }
      }
    }
    , $list
  }
  $inv.power = Invoke-DiagSafe -What 'power plan' -Errors $errors -Block {
    $out = (& powercfg.exe /getactivescheme 2>$null) -join ' '
    $guid = ''
    $name = ''
    $m = [regex]::Match($out, '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})')
    if ($m.Success) { $guid = $m.Groups[1].Value.ToLowerInvariant() }
    $m2 = [regex]::Match($out, '\(([^)]*)\)\s*$')
    if ($m2.Success) { $name = $m2.Groups[1].Value }
    $known = @{
      '381b4222-f694-41f0-9685-ff5bb260df2e' = 'Balanced'
      '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c' = 'High performance'
      'a1841308-3541-4fab-bc81-f71556f20b4a' = 'Power saver'
      'e9a42b02-d5df-448d-aa00-03f14749eb61' = 'Ultimate performance'
    }
    $battery = $null
    try { $battery = @(Get-DiagCim Win32_Battery).Count -gt 0 } catch { }
    [ordered]@{ guid = $guid; name = $name; known_as = $known[$guid]; has_battery = $battery }
  }
  $inv.defender = Invoke-DiagSafe -What 'defender' -Errors $errors -Block {
    $d = [ordered]@{}
    if (Get-Command Get-MpComputerStatus -ErrorAction SilentlyContinue) {
      $s = Get-MpComputerStatus -ErrorAction Stop
      $d.service_enabled = [bool]$s.AMServiceEnabled
      $d.antivirus_enabled = [bool]$s.AntivirusEnabled
      $d.realtime = [bool]$s.RealTimeProtectionEnabled
      $d.on_access = [bool]$s.OnAccessProtectionEnabled
      $d.behavior_monitor = [bool]$s.BehaviorMonitorEnabled
      $d.running_mode = [string]$s.AMRunningMode
      $d.product_version = [string]$s.AMProductVersion
      try {
        $pref = Get-MpPreference -ErrorAction Stop
        $d.exclusion_path = @($pref.ExclusionPath | Where-Object { $_ } | ForEach-Object { [string]$_ })
        $d.exclusion_process = @($pref.ExclusionProcess | Where-Object { $_ } | ForEach-Object { [string]$_ })
        $d.exclusion_extension = @($pref.ExclusionExtension | Where-Object { $_ } | ForEach-Object { [string]$_ })
        $joined = (@($d.exclusion_path) + @($d.exclusion_process)) -join ' '
        $d.exclusions_readable = -not ($joined -match 'N/A|administrator')
      } catch {
        $d.exclusions_readable = $false
        $d.exclusions_error = $_.Exception.Message
      }
    } else {
      $d.note = 'Defender cmdlets not available'
    }
    $d
  }
  $inv.antivirus_products = Invoke-DiagSafe -What 'security center' -Errors $errors -Block {
    $list = @()
    foreach ($a in (Get-DiagCim -Namespace 'root/SecurityCenter2' -Class AntiVirusProduct)) {
      $state = [int]$a.productState
      $hex = $state.ToString('X6')
      $list += [ordered]@{
        name     = [string]$a.displayName
        state    = $hex
        enabled  = ($hex.Substring(2, 2) -eq '10' -or $hex.Substring(2, 2) -eq '11')
        uptodate = ($hex.Substring(4, 2) -eq '00')
      }
    }
    , $list
  }
  $inv.display_adapters = Invoke-DiagSafe -What 'display adapters' -Errors $errors -Block {
    $list = @()
    foreach ($v in (Get-DiagCim Win32_VideoController)) {
      $list += [ordered]@{
        name           = [string]$v.Name
        driver_version = [string]$v.DriverVersion
        driver_date    = ConvertTo-DiagIso $v.DriverDate
        resolution     = $(if ($v.CurrentHorizontalResolution) { [string]$v.CurrentHorizontalResolution + 'x' + [string]$v.CurrentVerticalResolution } else { $null })
        refresh_hz     = $v.CurrentRefreshRate
        status         = [string]$v.Status
      }
    }
    , $list
  }
  $inv.monitors = Invoke-DiagSafe -What 'monitors' -Errors $errors -Block { Get-DiagMonitorInfo }
  $inv.pagefile = Invoke-DiagSafe -What 'page file' -Errors $errors -Block {
    $list = @()
    foreach ($pf in (Get-DiagCim Win32_PageFileUsage)) {
      $list += [ordered]@{ name = [string]$pf.Name; allocated_mb = $pf.AllocatedBaseSize; current_mb = $pf.CurrentUsage; peak_mb = $pf.PeakUsage }
    }
    , $list
  }
  $inv.processes = Invoke-DiagSafe -What 'processes' -Errors $errors -Block { Get-DiagProcessesOfInterest }
  if (-not $SkipTempScan) {
    $inv.temp = Invoke-DiagSafe -What 'temp scan' -Errors $errors -Block {
      Get-DirectoryStats -Path (Get-DiagTempRoot) -Recurse -MaxEntries $TempScanMaxEntries -MaxSeconds $TempScanSeconds
    }
  }
  $inv.errors = @($errors)
  return $inv
}

function Get-DiagPathInfo {
  param([string]$Path, $LogicalDisks, $PhysicalDisks, $Errors)
  $r = [ordered]@{ path = $Path; exists = $false; network = $false; unc = $false; drive = $null; drive_type = $null; target = $null; fs = $null; free_gb = $null; size_gb = $null; media_type = $null; files_top = $null; images_top = $null; count_complete = $null; ascii = (Test-DiagAsciiPath $Path) }
  try {
    $r.exists = Test-Path -LiteralPath $Path
    $full = $Path
    try { $full = [System.IO.Path]::GetFullPath($Path) } catch { }
    $r.path = $full
    if ($full.StartsWith('\\')) {
      $r.unc = $true
      $r.network = $true
      $m = [regex]::Match($full, '^\\\\[^\\]+\\[^\\]+')
      if ($m.Success) { $r.target = $m.Value }
      try {
        $fso = New-Object -ComObject Scripting.FileSystemObject
        $drv = $fso.GetDrive($r.target)
        $r.fs = [string]$drv.FileSystem
        $r.free_gb = [Math]::Round([double]$drv.FreeSpace / 1GB, 1)
        $r.size_gb = [Math]::Round([double]$drv.TotalSize / 1GB, 1)
      } catch { }
    } elseif ($full.Length -ge 2 -and $full[1] -eq ':') {
      $letter = $full.Substring(0, 2).ToUpperInvariant()
      $r.drive = $letter
      foreach ($ld in @($LogicalDisks)) {
        if ($null -eq $ld) { continue }
        if ([string]$ld.DeviceID -eq $letter) {
          $r.drive_type = @{ 2 = 'removable'; 3 = 'local'; 4 = 'network'; 5 = 'cd'; 6 = 'ramdisk' }[[int]$ld.DriveType]
          $r.network = ([int]$ld.DriveType -eq 4)
          if ($r.network) { $r.target = [string]$ld.ProviderName }
          $r.fs = [string]$ld.FileSystem
          $r.free_gb = [Math]::Round([double]$ld.FreeSpace / 1GB, 1)
          $r.size_gb = [Math]::Round([double]$ld.Size / 1GB, 1)
        }
      }
      if (-not $r.network -and (Get-Command Get-Partition -ErrorAction SilentlyContinue)) {
        try {
          $part = Get-Partition -DriveLetter $letter.Substring(0, 1) -ErrorAction Stop | Select-Object -First 1
          foreach ($pd in @($PhysicalDisks)) {
            if ($pd -and [string]$pd.number -eq [string]$part.DiskNumber) { $r.media_type = [string]$pd.media_type + ' / ' + [string]$pd.bus_type }
          }
        } catch { }
      }
    }
    if ($r.exists) {
      $st = Get-DirectoryStats -Path $full -MaxEntries 50000 -MaxSeconds 10 -ImagePattern '\.(tif|tiff|jpg|jpeg|png)$'
      $r.files_top = $st.files
      $r.images_top = $st.images
      $r.count_complete = $st.complete
    }
  } catch {
    if ($null -ne $Errors) { $Errors.Add('path ' + $Path + ': ' + $_.Exception.Message) }
  }
  return $r
}

function Get-DiagMonitorInfo {
  $r = [ordered]@{ screens = @(); log_pixels = $null; win8_dpi_scaling = $null; per_monitor = @() }
  try {
    # Without DPI awareness Windows reports scaled-down bounds on a scaled display.
    if (Initialize-DiagNative) { try { [void][StDiag.Native]::SetProcessDPIAware() } catch { } }
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
      $r.screens += [ordered]@{ name = [string]$s.DeviceName; primary = [bool]$s.Primary; bounds = ([string]$s.Bounds.Width + 'x' + [string]$s.Bounds.Height); bits = $s.BitsPerPixel }
    }
  } catch { }
  try {
    $desk = Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -ErrorAction Stop
    $r.log_pixels = $desk.LogPixels
    $r.win8_dpi_scaling = $desk.Win8DpiScaling
  } catch { }
  try {
    $key = 'HKCU:\Control Panel\Desktop\PerMonitorSettings'
    if (Test-Path -LiteralPath $key) {
      foreach ($k in @(Get-ChildItem -LiteralPath $key -ErrorAction Stop)) {
        $v = Get-ItemProperty -LiteralPath $k.PSPath -ErrorAction SilentlyContinue
        $r.per_monitor += [ordered]@{ monitor = [string]$k.PSChildName; dpi_value = $v.DpiValue }
      }
    }
  } catch { }
  $r.note = 'DpiValue is an offset from the monitor''s recommended scaling (0 = recommended). The application''s own screen records carry the effective device pixel ratio.'
  return $r
}

function Get-DiagProcessesOfInterest {
  $groups = @{}
  foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
    $n = $p.ProcessName.ToLowerInvariant()
    $cat = $null
    if ($n -like 'scantailor*') { $cat = 'scantailor' }
    elseif ($script:ProcessesOfInterest.ContainsKey($n)) { $cat = $script:ProcessesOfInterest[$n] }
    elseif ($n -like 'acronis*' -or $n -like 'veeam*') { $cat = 'backup' }
    if (-not $cat) { continue }
    $key = $cat + '|' + $n
    if (-not $groups.ContainsKey($key)) {
      $groups[$key] = [ordered]@{ category = $cat; name = $p.ProcessName; count = 0; ws_mb = 0.0; cpu_s = $null; pids = @(); paths = @() }
    }
    $g = $groups[$key]
    $g.count++
    $g.ws_mb += [Math]::Round($p.WorkingSet64 / 1MB, 1)
    $g.pids += $p.Id
    try {
      # Protected processes (antivirus) hide their CPU time from a normal user.
      $cpu = $p.TotalProcessorTime
      if ($null -ne $cpu) {
        if ($null -eq $g.cpu_s) { $g.cpu_s = 0.0 }
        $g.cpu_s = [Math]::Round($g.cpu_s + $cpu.TotalSeconds, 1)
      }
    } catch { }
    if ($cat -eq 'scantailor') {
      try { if ($p.Path) { $g.paths += $p.Path } } catch { }
    }
  }
  $list = @()
  foreach ($k in ($groups.Keys | Sort-Object)) { $list += $groups[$k] }
  return , $list
}

# ---------------------------------------------------------------------------
# Storage micro-benchmark
# ---------------------------------------------------------------------------

function New-DiagTimingStats {
  param($List)
  $s = Get-SampleStats -Values $List
  return [ordered]@{
    n    = $s.n
    p50  = if ($null -ne $s.p50) { [Math]::Round($s.p50, 3) } else { $null }
    p95  = if ($null -ne $s.p95) { [Math]::Round($s.p95, 3) } else { $null }
    max  = if ($null -ne $s.max) { [Math]::Round($s.max, 3) } else { $null }
    mean = if ($null -ne $s.mean) { [Math]::Round($s.mean, 3) } else { $null }
  }
}

function Invoke-DiagReplace {
  <# Replaces $Target with $Source the way the application does (MoveFileEx, replace existing). #>
  param([string]$Source, [string]$Target)
  if ($script:NativeOk) {
    if (-not [StDiag.Native]::MoveFileEx($Source, $Target, 1)) {
      throw (New-Object System.ComponentModel.Win32Exception([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))
    }
    return
  }
  [System.IO.File]::Replace($Source, $Target, $null)
}

function Test-DiagNetworkPath {
  <#
    $true for a UNC path or a mapped network drive, $false for a local drive,
    $null when it cannot be told. Only reads the path; nothing is created.
  #>
  param([string]$Path)
  try {
    if (-not $Path) { return $null }
    if ($Path.StartsWith('\\?\UNC\', [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
    $p = $Path
    if ($p.StartsWith('\\?\') -or $p.StartsWith('\\.\')) { $p = $p.Substring(4) }
    elseif ($p.StartsWith('\\') -or $p.StartsWith('//')) { return $true }
    $full = [System.IO.Path]::GetFullPath($p)
    if ($full.StartsWith('\\')) { return $true }
    $root = [System.IO.Path]::GetPathRoot($full)
    if (-not $root) { return $null }
    $dt = (New-Object System.IO.DriveInfo($root)).DriveType
    if ($dt -eq [System.IO.DriveType]::Network) { return $true }
    if ($dt -eq [System.IO.DriveType]::Unknown -or $dt -eq [System.IO.DriveType]::NoRootDirectory) { return $null }
    return $false
  } catch {
    return $null
  }
}

function Measure-Storage {
  <#
    Times the file operations Scantailor-DGI depends on, in a temporary
    subdirectory of $Path that is removed afterwards (when that fails, for
    example on a share that lets a user create but not delete, the folder's
    path is reported in .errors so that it can be removed by hand):
      small_create  create + write 4 KB + close (thumbnails, caches, small outputs)
      small_delete  deleting those files
      stat          reading attributes of an existing file / probing a missing one
      durable_1mb   1 MB write-through + FlushFileBuffers (the project save path)
      rename        replace an existing file by rename (the atomic save's last step)
      seq_write / seq_read  large sequential throughput (output TIFFs)
    -ReadOnly (or an unwritable $Path) measures only stat and read of files that
    are already there, which is all the application does to a scans folder.
  #>
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Label = '',
    [string]$Role = 'probe',
    [switch]$Quick,
    [int]$SeqMB = 256,
    [double]$MaxSeqSeconds = 15,
    [switch]$ReadOnly
  )
  $nSmall = 200; $nDurable = 20; $nStat = 200; $nRename = 50
  if ($Quick) { $nSmall = 50; $nDurable = 5; $nStat = 50; $nRename = 15; $SeqMB = [Math]::Min($SeqMB, 64); $MaxSeqSeconds = [Math]::Min($MaxSeqSeconds, 5) }
  # A relative path means relative to PowerShell's current location, which the
  # .NET calls below would otherwise resolve against the process directory.
  try { $Path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path) } catch { }
  $res = [ordered]@{
    label    = $Label
    role     = $Role
    path     = $Path
    started  = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss', $script:Inv)
    mode     = 'full'
    quick    = [bool]$Quick
    network  = (Test-DiagNetworkPath $Path)
    tests    = [ordered]@{}
    notes    = @()
    errors   = @()
    seconds  = 0.0
  }
  $total = [System.Diagnostics.Stopwatch]::StartNew()
  if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
    $res.errors += 'Path does not exist or is not a directory.'
    $res.mode = 'none'
    return $res
  }
  [void](Initialize-DiagNative)
  $sw = New-Object System.Diagnostics.Stopwatch
  $rng = New-Object System.Random(12345)
  $probe = $null
  if (-not $ReadOnly) {
    try {
      $probe = Join-Path $Path ('st-diag-probe-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
      [void][System.IO.Directory]::CreateDirectory($probe)
      $t = Join-Path $probe 'w.tmp'
      [System.IO.File]::WriteAllBytes($t, (New-Object byte[] 16))
      [System.IO.File]::Delete($t)
    } catch {
      $res.notes += ('Not writable (' + $_.Exception.Message + '); read-only tests only.')
      # A share can allow creating a folder but not deleting in it: then the
      # probe folder stays behind and the operator must be told where.
      if ($probe -and [System.IO.Directory]::Exists($probe)) {
        try { [System.IO.Directory]::Delete($probe, $true) } catch { $res.errors += ('cleanup: could not remove the probe folder ' + $probe + ' (' + $_.Exception.Message + '); remove it by hand.') }
      }
      $probe = $null
      $ReadOnly = $true
    }
  }
  if ($ReadOnly) {
    $res.mode = 'read-only'
    Measure-DiagStorageReadOnly -Path $Path -Result $res -NStat $nStat -MaxMB $SeqMB -MaxSeconds $MaxSeqSeconds
    $res.seconds = [Math]::Round($total.Elapsed.TotalSeconds, 1)
    return $res
  }

  try {
    $small = New-Object byte[] 4096
    $rng.NextBytes($small)
    $files = New-Object System.Collections.Generic.List[string]

    # (a) small files
    $tCreate = New-Object System.Collections.Generic.List[double]
    try {
      for ($i = 0; $i -lt $nSmall; $i++) {
        $f = Join-Path $probe ('s' + $i + '.bin')
        $sw.Restart()
        $fs = [System.IO.FileStream]::new($f, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None, 4096)
        try { $fs.Write($small, 0, 4096); $fs.Flush() } finally { $fs.Dispose() }
        $sw.Stop()
        $tCreate.Add($sw.Elapsed.TotalMilliseconds)
        $files.Add($f)
      }
    } catch { $res.errors += ('small_create: ' + $_.Exception.Message) }
    $res.tests.small_create = New-DiagTimingStats $tCreate

    # (d) metadata lookups on the files just made, and on missing ones
    $tStat = New-Object System.Collections.Generic.List[double]
    $tMiss = New-Object System.Collections.Generic.List[double]
    if ($files.Count -gt 0) {
      try {
        for ($i = 0; $i -lt $nStat; $i++) {
          $f = $files[$i % $files.Count]
          $sw.Restart()
          [void][System.IO.File]::GetAttributes($f)
          [void][System.IO.File]::GetLastWriteTimeUtc($f)
          $sw.Stop()
          $tStat.Add($sw.Elapsed.TotalMilliseconds)
        }
        for ($i = 0; $i -lt $nStat; $i++) {
          $f = Join-Path $probe ('missing' + $i + '.bin')
          $sw.Restart()
          [void][System.IO.File]::Exists($f)
          $sw.Stop()
          $tMiss.Add($sw.Elapsed.TotalMilliseconds)
        }
      } catch { $res.errors += ('stat: ' + $_.Exception.Message) }
    }
    $res.tests.stat = New-DiagTimingStats $tStat
    $res.tests.stat_missing = New-DiagTimingStats $tMiss

    $tDelete = New-Object System.Collections.Generic.List[double]
    try {
      foreach ($f in $files) {
        $sw.Restart()
        [System.IO.File]::Delete($f)
        $sw.Stop()
        $tDelete.Add($sw.Elapsed.TotalMilliseconds)
      }
    } catch { $res.errors += ('small_delete: ' + $_.Exception.Message) }
    $res.tests.small_delete = New-DiagTimingStats $tDelete

    # (b) durable 1 MB writes
    $mb = New-Object byte[] 1048576
    $rng.NextBytes($mb)
    $tDurable = New-Object System.Collections.Generic.List[double]
    try {
      for ($i = 0; $i -lt $nDurable; $i++) {
        $f = Join-Path $probe ('d' + $i + '.bin')
        $sw.Restart()
        $fs = [System.IO.FileStream]::new($f, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None, 65536, [System.IO.FileOptions]::WriteThrough)
        try { $fs.Write($mb, 0, $mb.Length); $fs.Flush($true) } finally { $fs.Dispose() }
        $sw.Stop()
        $tDurable.Add($sw.Elapsed.TotalMilliseconds)
        [System.IO.File]::Delete($f)
      }
    } catch { $res.errors += ('durable_1mb: ' + $_.Exception.Message) }
    $res.tests.durable_1mb = New-DiagTimingStats $tDurable

    # (e) rename over an existing file
    $tRename = New-Object System.Collections.Generic.List[double]
    try {
      $target = Join-Path $probe 'target.bin'
      [System.IO.File]::WriteAllBytes($target, $small)
      for ($i = 0; $i -lt $nRename; $i++) {
        $tmp = Join-Path $probe ('r' + $i + '.tmp')
        [System.IO.File]::WriteAllBytes($tmp, $small)
        $sw.Restart()
        Invoke-DiagReplace -Source $tmp -Target $target
        $sw.Stop()
        $tRename.Add($sw.Elapsed.TotalMilliseconds)
      }
      [System.IO.File]::Delete($target)
    } catch { $res.errors += ('rename: ' + $_.Exception.Message) }
    $res.tests.rename = New-DiagTimingStats $tRename
    if ($script:NativeOk) { $res.tests.rename.method = 'MoveFileEx' } else { $res.tests.rename.method = 'File.Replace' }

    # (c) sequential throughput, bounded in time so a slow share cannot stall the run
    $block = New-Object byte[] 4194304
    $rng.NextBytes($block)
    $big = Join-Path $probe 'seq.bin'
    try {
      $written = [long]0
      $sw.Restart()
      $fs = [System.IO.FileStream]::new($big, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None, 1048576)
      try {
        while ($written -lt ([long]$SeqMB * 1048576) -and $sw.Elapsed.TotalSeconds -lt $MaxSeqSeconds) {
          $fs.Write($block, 0, $block.Length)
          $written += $block.Length
        }
        $fs.Flush($true)
      } finally { $fs.Dispose() }
      $sw.Stop()
      $wsec = $sw.Elapsed.TotalSeconds
      $res.tests.seq_write = [ordered]@{ mb = [Math]::Round($written / 1048576, 0); seconds = [Math]::Round($wsec, 2); mb_per_s = [Math]::Round(($written / 1048576) / [Math]::Max($wsec, 0.001), 1); capped = ($written -lt ([long]$SeqMB * 1048576)) }

      $read = [long]0
      $sw.Restart()
      $fs = [System.IO.FileStream]::new($big, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read, 1048576, [System.IO.FileOptions]::SequentialScan)
      try {
        while ($sw.Elapsed.TotalSeconds -lt $MaxSeqSeconds) {
          $k = $fs.Read($block, 0, $block.Length)
          if ($k -le 0) { break }
          $read += $k
        }
      } finally { $fs.Dispose() }
      $sw.Stop()
      $rsec = $sw.Elapsed.TotalSeconds
      $res.tests.seq_read = [ordered]@{ mb = [Math]::Round($read / 1048576, 0); seconds = [Math]::Round($rsec, 2); mb_per_s = [Math]::Round(($read / 1048576) / [Math]::Max($rsec, 0.001), 1); cached = $true }
      $res.notes += 'seq_read re-reads the file just written, so it is usually served from the Windows file cache: treat it as an upper bound.'
    } catch { $res.errors += ('sequential: ' + $_.Exception.Message) }
  } finally {
    if ($probe) {
      try { [System.IO.Directory]::Delete($probe, $true) } catch { $res.errors += ('cleanup: could not remove the probe folder ' + $probe + ' (' + $_.Exception.Message + '); remove it by hand.') }
    }
  }
  $res.seconds = [Math]::Round($total.Elapsed.TotalSeconds, 1)
  return $res
}

function Measure-DiagStorageReadOnly {
  param([string]$Path, $Result, [int]$NStat, [int]$MaxMB, [double]$MaxSeconds)
  $files = New-Object System.Collections.Generic.List[string]
  $images = New-Object System.Collections.Generic.List[string]
  try {
    foreach ($f in [System.IO.Directory]::EnumerateFiles($Path)) {
      $files.Add($f)
      if ($f -match '\.(tif|tiff|jpg|jpeg|png)$') { $images.Add($f) }
      if ($files.Count -ge 2000) { break }
    }
  } catch { $Result.errors += ('enumerate: ' + $_.Exception.Message) }
  if ($files.Count -eq 0) {
    $Result.notes += 'No files to measure.'
    return
  }
  $sw = New-Object System.Diagnostics.Stopwatch
  $tStat = New-Object System.Collections.Generic.List[double]
  try {
    for ($i = 0; $i -lt $NStat; $i++) {
      $f = $files[$i % $files.Count]
      $sw.Restart()
      [void][System.IO.File]::GetAttributes($f)
      [void][System.IO.File]::GetLastWriteTimeUtc($f)
      $sw.Stop()
      $tStat.Add($sw.Elapsed.TotalMilliseconds)
    }
  } catch { $Result.errors += ('stat: ' + $_.Exception.Message) }
  $Result.tests.stat = New-DiagTimingStats $tStat
  $src = $images
  if ($src.Count -eq 0) { $src = $files }
  $buf = New-Object byte[] 4194304
  $read = [long]0
  $nfiles = 0
  $sw.Restart()
  try {
    foreach ($f in $src) {
      $fs = [System.IO.FileStream]::new($f, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite, Delete', 1048576, [System.IO.FileOptions]::SequentialScan)
      try {
        while ($true) {
          $k = $fs.Read($buf, 0, $buf.Length)
          if ($k -le 0) { break }
          $read += $k
          if ($read -ge ([long]$MaxMB * 1048576) -or $sw.Elapsed.TotalSeconds -ge $MaxSeconds) { break }
        }
      } finally { $fs.Dispose() }
      $nfiles++
      if ($read -ge ([long]$MaxMB * 1048576) -or $sw.Elapsed.TotalSeconds -ge $MaxSeconds) { break }
    }
  } catch { $Result.errors += ('read: ' + $_.Exception.Message) }
  $sw.Stop()
  $sec = $sw.Elapsed.TotalSeconds
  $Result.tests.seq_read = [ordered]@{ mb = [Math]::Round($read / 1048576, 0); files = $nfiles; seconds = [Math]::Round($sec, 2); mb_per_s = [Math]::Round(($read / 1048576) / [Math]::Max($sec, 0.001), 1); cached = $false }
  $Result.notes += 'Read test used existing files; if they were read recently the Windows cache may make it look faster than a first read.'
}

function Get-StorageHints {
  <# Plain-language reading of a Measure-Storage result. #>
  param($Result)
  $h = @()
  if ($null -eq $Result -or $null -eq $Result.tests) { return , $h }
  $t = $Result.tests
  # Whether the location is a network one decides what a slow result points
  # to: on a local disk it is not network latency but a filter driver
  # (antivirus/EDR) or the disk itself. Results from older versions of the
  # suite have no "network" field; then both causes are named.
  $net = $null
  if ($Result -is [System.Collections.IDictionary]) { if ($Result.Contains('network')) { $net = $Result['network'] } }
  elseif ($Result.PSObject.Properties['network']) { $net = $Result.network }
  if ($t.durable_1mb -and $null -ne $t.durable_1mb.p50 -and [double]$t.durable_1mb.p50 -gt 20) {
    $why = 'a network share, an HDD or a disk without write-back caching'
    if ($null -ne $net) {
      if ($net) { $why = 'the network share (every flush is a round trip to the server)' }
      else { $why = 'an HDD, a disk without write-back caching, or antivirus/EDR scanning each write (this is a local disk)' }
    }
    $h += ('A durable 1 MB write takes ' + (Format-DiagMs $t.durable_1mb.p50) + ' (median). Above ~20 ms usually means ' + $why + '; every project save waits for this.')
  }
  if ($t.small_create -and $null -ne $t.small_create.p95 -and [double]$t.small_create.p95 -gt 10) {
    $why = 'Typical of network shares or of real-time antivirus scanning each new file.'
    if ($null -ne $net) {
      if ($net) { $why = 'Typical of a network share: every create is a round trip to the server (antivirus on the server or here adds to it).' }
      else { $why = 'This is a local disk, so it points to real-time antivirus/EDR scanning each new file or another file-system filter driver.' }
    }
    $h += ('Creating a small file takes up to ' + (Format-DiagMs $t.small_create.p95) + ' (p95). ' + $why)
  }
  if ($t.stat -and $null -ne $t.stat.p50 -and [double]$t.stat.p50 -gt 1) {
    $why = 'network latency, or an antivirus/EDR or other file-system filter driver intercepting every lookup'
    if ($null -ne $net) {
      if ($net) { $why = 'network latency' }
      else { $why = 'this is a local disk, so not network latency: an antivirus/EDR or other file-system filter driver is intercepting every lookup' }
    }
    $h += ('Reading file attributes takes ' + (Format-DiagMs $t.stat.p50) + ' (median): ' + $why + '. The application checks existence and timestamps often, some of it on the GUI thread.')
  }
  if ($t.rename -and $null -ne $t.rename.p95 -and [double]$t.rename.p95 -gt 20) {
    $h += ('Replacing a file by rename takes up to ' + (Format-DiagMs $t.rename.p95) + ' (p95); the last step of every project save.')
  }
  if ($t.seq_write -and $null -ne $t.seq_write.mb_per_s -and [double]$t.seq_write.mb_per_s -lt 60) {
    $h += ('Sequential write ' + (Format-DiagNumber $t.seq_write.mb_per_s 1) + ' MB/s: slow for writing output TIFFs (a 300 dpi A4 colour page is ~25 MB uncompressed).')
  }
  if ($t.seq_read -and $null -ne $t.seq_read.mb_per_s -and -not $t.seq_read.cached -and [double]$t.seq_read.mb_per_s -lt 60) {
    $h += ('Reading existing files runs at ' + (Format-DiagNumber $t.seq_read.mb_per_s 1) + ' MB/s: loading scans from here is slow.')
  }
  return , $h
}

Export-ModuleMember -Function *
