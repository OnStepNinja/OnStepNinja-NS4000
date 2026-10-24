# Find the safe GOTO step rate: for each rate, GOTO both axes over Wi-Fi while polling like SkySafari, then stop.
# ASCII only (Windows PowerShell 5.1). Motors must be free (not on the mount).
# Usage: rate_sweep.ps1 -Ip 192.168.4.1 -Kpps "20,30,40" -Seconds 30
param([string]$Ip = '192.168.4.1', [string]$Kpps = '20,30,40', [int]$Seconds = 30)
$rates = $Kpps.Split(',') | ForEach-Object { [double]$_ }
if ($rates | Where-Object { $_ -lt 5 -or $_ -gt 70 }) { throw 'Kpps must be 5..70 (comma separated, e.g. 20,30,40)' }
$ErrorActionPreference = 'Stop'
function Conn($port) { $c = New-Object Net.Sockets.TcpClient; $c.NoDelay = $true; $t = $c.ConnectAsync($Ip, $port); if (-not $t.Wait(3000)) { throw "connect $port timeout" }; $c }
function TQ($cl, [string]$cmd, [int]$wait = 1500) {
  $st = $cl.GetStream(); $b = [Text.Encoding]::ASCII.GetBytes($cmd); $sw = [Diagnostics.Stopwatch]::StartNew(); $st.Write($b, 0, $b.Length)
  $r = ''; $buf = New-Object byte[] 256
  while ($sw.ElapsedMilliseconds -lt $wait) {
    while ($st.DataAvailable) { $n = $st.Read($buf, 0, 256); $r += [Text.Encoding]::ASCII.GetString($buf, 0, $n) }
    if ($r.EndsWith('#')) { break }
    if (($cmd -match '^:(MS|Te|Sr|Sd|SX92|Q)') -and $r.Length -gt 0) { break }
    Start-Sleep -Milliseconds 1
  }
  [pscustomobject]@{ r = $r; ms = $sw.ElapsedMilliseconds }
}
function HMS([double]$h) { $h = (($h % 24) + 24) % 24; $t = [int][Math]::Round($h * 3600); '{0:00}:{1:00}:{2:00}' -f [int][Math]::Floor($t / 3600), [int][Math]::Floor(($t % 3600) / 60), ($t % 60) }
$results = @()
foreach ($k in $rates) {
  $us = 1000.0 / $k   # microseconds per step on axis 1
  $res = [ordered]@{ kpps = $k; us = [Math]::Round($us, 3); result = 'OK'; worstReplyMs = 0; noReply = 0; FAmax = 0; FCmax = 0; GX92 = '' }
  try {
    $mon = Conn 9998
    $null = TQ $mon ':Q#'; Start-Sleep 3
    $null = TQ $mon (':SX92,' + ('{0:F3}' -f $us) + '#'); $res.GX92 = (TQ $mon ':GX92#').r
    # go home first so every run covers the same path
    $null = TQ $mon ':hC#'; $sw = [Diagnostics.Stopwatch]::StartNew()
    do { Start-Sleep -Milliseconds 500; $gu = (TQ $mon ':GU#').r } while (-not $gu.Contains('H') -and $sw.Elapsed.TotalSeconds -lt 400)
    $sky = Conn 9999   # open after homing (the server closes connections idle for 30 s)
    $lst = (TQ $sky ':GS#').r.TrimEnd('#').Split(':'); $lstH = [double]$lst[0] + [double]$lst[1] / 60 + [double]$lst[2] / 3600
    $null = TQ $sky ':Te#'; $null = TQ $sky (':Sr' + (HMS ($lstH - 1)) + '#'); $null = TQ $sky ':Sd+40*00:00#'
    $ms = (TQ $sky ':MS#').r
    if ($ms -ne '0') { $res.result = 'GOTO refused [' + $ms + ']' } else {
      $null = TQ $mon ':GXFA#'
      $t0 = [Diagnostics.Stopwatch]::StartNew(); $i = 0
      while ($t0.Elapsed.TotalSeconds -lt $Seconds) {
        foreach ($c in ':GR#', ':GD#') {
          $x = TQ $sky $c
          if ($x.ms -gt $res.worstReplyMs) { $res.worstReplyMs = $x.ms }
          if (-not $x.r.EndsWith('#')) { $res.noReply++ }
        }
        if ($res.noReply -ge 3) { $res.result = 'HANG'; break }
        $i++
        if ($i % 15 -eq 0) {
          $fa = (TQ $mon ':GXFA#').r.TrimEnd('#').TrimEnd('%'); $fc = (TQ $mon ':GXFC#').r.TrimEnd('#').Replace('us', '')
          if ($fa -match '^\d+$' -and [int]$fa -gt $res.FAmax) { $res.FAmax = [int]$fa }
          if ($fc -match '^\d+$' -and [int]$fc -gt $res.FCmax) { $res.FCmax = [int]$fc }
        }
        Start-Sleep -Milliseconds 80
      }
      if ($res.result -eq 'OK') { $null = TQ $mon ':Q#' }
    }
    $sky.Close(); $mon.Close()
  } catch { $res.result = 'ERROR ' + $_.Exception.Message }
  $o = [pscustomobject]$res; $results += $o; $o | Format-Table -AutoSize | Out-String -Width 200
  if ($res.result -eq 'HANG') { 'waiting 20 s for the watchdog reboot ...'; Start-Sleep 20 }
}
$results | Format-Table -AutoSize | Out-String -Width 200
