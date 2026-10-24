# Measure RA step-interrupt lateness on OnStep NS-3000 (Ver4.25j09 or later). ASCII only (Windows PowerShell 5.1).
# Usage: jitter.ps1 -Port COM6 -Seconds 60
# Sends only :GVN# :=V# :SXJ0,1# :GXJ0# :GXJ1# :GXJ2# :GXFA# :GXFC# :SXJ0,0# (no motion, no NV writes).
param([string]$Port = 'COM6', [int]$Seconds = 60)
$ErrorActionPreference = 'Stop'
$sp = New-Object System.IO.Ports.SerialPort $Port, 9600, 'None', 8, 'One'
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.Open(); Start-Sleep -Milliseconds 800; $null = $sp.ReadExisting()
function Q([string]$c) { $null = $sp.ReadExisting(); $sp.Write($c); Start-Sleep -Milliseconds 300; $sp.ReadExisting() }
try {
  'version: ' + (Q ':=V#') + '  ' + (Q ':GVN#')
  $r = Q ':SXJ0,1#'
  if ($r -ne '1') { 'This firmware does not support :SXJ0 (reply [' + $r + ']).'; return }
  $null = Q ':GXFA#'   # reset worst loop time
  'measuring for ' + $Seconds + ' s ...'
  Start-Sleep -Seconds $Seconds
  $j0 = (Q ':GXJ0#').TrimEnd('#').Split(','); $j1 = (Q ':GXJ1#').TrimEnd('#').Split(','); $j2 = (Q ':GXJ2#').TrimEnd('#').Split(',')
  $fa = Q ':GXFA#'; $fc = Q ':GXFC#'
  $null = Q ':SXJ0,0#'
  'interrupts : ' + $j0[0]
  'late avg   : ' + $j0[1] + ' us'
  'late max   : ' + $j0[2] + ' us'
  'histogram  : <2us=' + $j1[0] + '  <10us=' + $j1[1] + '  <50us=' + $j1[2] + '  <200us=' + $j2[0] + '  <1000us=' + $j2[1] + '  >=1000us=' + $j2[2]
  'loop worst : ' + $fa + '   loop avg: ' + $fc
} finally { $sp.Close() }
