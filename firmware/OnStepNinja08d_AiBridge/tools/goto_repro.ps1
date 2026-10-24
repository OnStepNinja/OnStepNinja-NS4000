# Reproduce a SkySafari-like GOTO over Wi-Fi TCP and record reply times. ASCII only (Windows PowerShell 5.1).
# Port 9999 = "SkySafari" (target, :MS#, then poll :GR#/:GD#), port 9998 = monitor (:GXW4# :GXFA# :GU#).
# Usage: goto_repro.ps1 -Ip 192.168.4.1 -HoursWest 1 -Dec 40 -Out goto_repro.csv
param([string]$Ip = '192.168.4.1', [double]$HoursWest = 1.0, [int]$Dec = 40, [int]$MaxSeconds = 240, [string]$Out = 'goto_repro.csv')
$ErrorActionPreference = 'Stop'
function Conn($port) { $c = New-Object Net.Sockets.TcpClient; $c.NoDelay = $true; $c.Connect($Ip, $port); $c }
function TQ($cl, [string]$cmd, [int]$wait = 2000) {
  $st = $cl.GetStream(); $b = [Text.Encoding]::ASCII.GetBytes($cmd); $sw = [Diagnostics.Stopwatch]::StartNew(); $st.Write($b, 0, $b.Length)
  $r = ''; $buf = New-Object byte[] 256
  while ($sw.ElapsedMilliseconds -lt $wait) {
    while ($st.DataAvailable) { $n = $st.Read($buf, 0, 256); $r += [Text.Encoding]::ASCII.GetString($buf, 0, $n) }
    if ($r.EndsWith('#')) { break }
    if (($cmd -match '^:(MS|Te|Sr|Sd)') -and $r.Length -gt 0) { break }
    Start-Sleep -Milliseconds 1
  }
  [pscustomobject]@{ r = $r; ms = $sw.ElapsedMilliseconds }
}
function HMS([double]$h) { $h = (($h % 24) + 24) % 24; $t = [int][Math]::Round($h * 3600); '{0:00}:{1:00}:{2:00}' -f [int][Math]::Floor($t / 3600), [int][Math]::Floor(($t % 3600) / 60), ($t % 60) }
$t0 = [Diagnostics.Stopwatch]::StartNew()
function Log([string]$s) { '{0,7:N2}  {1}' -f $t0.Elapsed.TotalSeconds, $s }
$rows = New-Object System.Collections.ArrayList
$sky = Conn 9999; $mon = Conn 9998
Log ('monitor GXW4 ' + (TQ $mon ':GXW4#').r + '  GU ' + (TQ $mon ':GU#').r)
$lst = (TQ $sky ':GS#').r.TrimEnd('#').Split(':'); $lstH = [double]$lst[0] + [double]$lst[1] / 60 + [double]$lst[2] / 3600
$ra = HMS ($lstH - $HoursWest)
foreach ($c in ':GR#', ':GD#', ':Te#', (':Sr' + $ra + '#'), (':Sd+' + $Dec + '*00:00#')) { $x = TQ $sky $c; Log ("$c -> [" + $x.r + '] ' + $x.ms + 'ms') }
$x = TQ $sky ':MS#'; Log (':MS# -> [' + $x.r + '] ' + $x.ms + 'ms')
$worst = 0; $timeouts = 0; $i = 0; $slewing = $true
while ($t0.Elapsed.TotalSeconds -lt $MaxSeconds) {
  foreach ($c in ':GR#', ':GD#') {
    $x = TQ $sky $c
    if ($x.ms -gt $worst) { $worst = $x.ms }
    if (-not $x.r.EndsWith('#')) { $timeouts++; Log ("NO REPLY to $c after " + $x.ms + 'ms [' + $x.r + ']') }
    $null = $rows.Add([pscustomobject]@{ t = [Math]::Round($t0.Elapsed.TotalSeconds, 2); cmd = $c; ms = $x.ms; reply = $x.r })
  }
  $i++
  if ($i % 20 -eq 0) {
    $gu = (TQ $mon ':GU#'); $w4 = (TQ $mon ':GXW4#'); $fa = (TQ $mon ':GXFA#')
    Log ('GU ' + $gu.r + ' (' + $gu.ms + 'ms)  GXW4 ' + $w4.r + '  FA ' + $fa.r + '  worst ' + $worst + 'ms  noReply ' + $timeouts)
    if ($gu.r -match 'N' -and $gu.r.EndsWith('#') -and $i -gt 40) { Log 'slew finished'; break }
    if (-not $gu.r.EndsWith('#')) { Log 'MONITOR GOT NO REPLY'; }
  }
  Start-Sleep -Milliseconds 80
}
Log ('end: worst ' + $worst + 'ms, noReply ' + $timeouts + ', GXW4 ' + (TQ $mon ':GXW4#').r + ' GXW5 ' + (TQ $mon ':GXW5#').r)
$rows | Export-Csv -NoTypeInformation -Encoding ascii $Out
$sky.Close(); $mon.Close()
