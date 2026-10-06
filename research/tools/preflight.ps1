<#
  preflight.ps1 - read-only readiness check for the GL310 bus capture.

  Changes nothing. Run from the repo root in any PowerShell (Administrator
  gives a more complete driver listing):

      powershell -ExecutionPolicy Bypass -File tools\preflight.ps1

  Each line is OK / -- (missing) / !! (needs attention).
#>
$ErrorActionPreference = 'SilentlyContinue'
$fail = 0
function Ok($m)   { Write-Host "  OK  $m" -ForegroundColor Green }
function Miss($m) { Write-Host "  --  $m" -ForegroundColor Yellow; $script:fail++ }
function Bad($m)  { Write-Host "  !!  $m" -ForegroundColor Red;    $script:fail++ }
function Info($m) { Write-Host "      $m" -ForegroundColor DarkGray }

Write-Host "`nGL310 capture preflight`n"

# --- Wireshark + USBPcap ----------------------------------------------------
$ws = Get-Command wireshark.exe -ErrorAction SilentlyContinue
if (-not $ws) { $ws = Get-Item "$env:ProgramFiles\Wireshark\Wireshark.exe" }
if ($ws) { Ok "Wireshark: $(if ($ws.Source) { $ws.Source } else { $ws.FullName })" } else { Miss "Wireshark not found" }

$usbpcapCmd = @(
  "$env:ProgramFiles\USBPcap\USBPcapCMD.exe",
  "$env:ProgramFiles\Wireshark\extcap\USBPcapCMD.exe",
  "$env:ProgramFiles\Wireshark\extcap\wireshark\USBPcapCMD.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
$svc = Get-Service USBPcap
if ($usbpcapCmd -and $svc) {
  Ok "USBPcap: $usbpcapCmd (service $($svc.Status))"
} elseif ($usbpcapCmd) {
  Bad "USBPcapCMD present but USBPcap driver service missing - reinstall USBPcap, then reboot"
} else {
  Miss "USBPcap not installed (Wireshark installer component, or desowin.org/usbpcap) - reboot after"
}

# Wireshark only lists USBPcap interfaces if USBPcapCMD.exe sits in its extcap folder.
# A Wireshark upgrade can drop it even though USBPcap itself is still installed.
$extcap = "$env:ProgramFiles\Wireshark\extcap\USBPcapCMD.exe"
if (Test-Path $extcap) { Ok "USBPcapCMD.exe is in Wireshark's extcap folder" }
elseif ($usbpcapCmd) { Bad "Wireshark can't see USBPcap - admin: copy `"$usbpcapCmd`" `"$env:ProgramFiles\Wireshark\extcap\`" then restart Wireshark" }

# --- DebugView --------------------------------------------------------------
$dbgv = @("$env:USERPROFILE\Downloads", "$env:USERPROFILE\Desktop", "$env:ProgramFiles", (Split-Path $PSScriptRoot))  |
  ForEach-Object { Get-ChildItem $_ -Recurse -Depth 3 -Filter 'Dbgview*.exe' } | Select-Object -First 1
if ($dbgv) { Ok "DebugView: $($dbgv.FullName)" } else { Miss "DebugView (Dbgview64.exe) not found in Downloads/Desktop/Program Files" }

# --- Debug Print Filter -----------------------------------------------------
$dpf = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter'
if ($dpf -and $null -ne $dpf.DEFAULT -and ('{0:X}' -f $dpf.DEFAULT) -eq 'FFFFFFFF') {
  Ok "Debug Print Filter DEFAULT = 0xFFFFFFFF (takes effect after a reboot)"
} elseif ($dpf -and $null -ne $dpf.DEFAULT) {
  Bad ("Debug Print Filter DEFAULT = 0x{0:X} - should be 0xFFFFFFFF" -f $dpf.DEFAULT)
} else {
  Miss "Debug Print Filter DEFAULT not set (admin: reg add ... /v DEFAULT /t REG_DWORD /d 0xFFFFFFFF /f, then reboot)"
}

# --- Last boot vs. setup ----------------------------------------------------
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
Info "last boot: $boot  (USBPcap and the print filter need a reboot after install)"

# --- AVerMedia driver -------------------------------------------------------
$drv = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match 'aver835' }
if (-not $drv) {
  $drv = (pnputil /enum-drivers) -join "`n" | Select-String -Pattern 'aver835_x64\.inf' -SimpleMatch
}
$sys = Test-Path "$env:SystemRoot\System32\drivers\AVer330USB.sys"
$fw  = (Test-Path "$env:SystemRoot\System32\drivers\qpvidfwusb.bin") -and (Test-Path "$env:SystemRoot\System32\drivers\qpaudfwusb.bin")
if ($drv -or $sys) {
  Ok "AVerMedia driver package staged$(if ($sys) {' (AVer330USB.sys present)'})"
  if (-not $fw) { Info "firmware .bin files not yet copied to System32\drivers (normal until first plug-in)" }
  # Must be the checked build whose firmware matches vendor\ (gl310cap.py fw relies on it)
  $want = @{
    'AVer330USB.sys' = '5476AF5F44DBE22E86AE656E7D3EC69961926FE1BBBF193C26B70A1490861DEC'
    'qpvidfwusb.bin' = '8DA30F6DC2D4FEE5AEB3E2E06DF5581DB4C01D146C50C21F6A5D4824025A47D1'
    'qpaudfwusb.bin' = '0E8EA66B6A3DDB68F9A283F10155DF90CE8D6F46599ED0ECF6F308D6C90210F4'
  }
  foreach ($f in $want.Keys) {
    $p = "$env:SystemRoot\System32\drivers\$f"
    if (-not (Test-Path $p)) { continue }
    if ((Get-FileHash $p).Hash -eq $want[$f]) { Ok "$f matches vendor\ (3.2802.64.40 checked build)" }
    else { Bad "$f differs from vendor\ - not the 3.2802.64.40 checked build" }
  }
} else {
  Miss "AVer835_x64.inf not in the driver store - run the archived installer (vendor\ lacks the .cat)"
}

# --- Anything else that might open the card -------------------------------
$aver = Get-Process | Where-Object { $_.Name -match 'AVer|StreamEngine|RECentral' -or $_.Company -match 'AVerMedia' }
$averSvc = Get-Service | Where-Object { $_.Status -eq 'Running' -and ($_.Name -match 'AVer' -or $_.DisplayName -match 'AVer|Stream Engine') -and $_.Name -ne 'AVer330USB' }
if ($aver -or $averSvc) {
  Bad ("AVerMedia software running - quit it before capturing: " + (@($aver.Name) + @($averSvc.Name) | Sort-Object -Unique) -join ', ')
} else { Ok "no AVerMedia apps/services running (Stream Engine is closed)" }

# --- Memory integrity (HVCI) -----------------------------------------------
$hvci = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
if ($hvci.Enabled -eq 1) {
  Info "Memory integrity is ON. If the card shows Code 39/52 after plug-in, that is the likely cause."
} else {
  Ok "Memory integrity off (old drivers load normally)"
}

# --- The card itself --------------------------------------------------------
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_07CA&PID_(C835|D835)' }
if ($dev) {
  foreach ($d in $dev) {
    $code = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode').Data
    $line = "GL310 present: $($d.FriendlyName) [$($d.Status)]$(if ($code) {" problem code $code"})"
    if ($d.Status -eq 'OK') { Ok $line } else { Bad $line }
  }
  Info "Stage 03 wants it plugged in (to find the hub); Stage 04 needs it UNPLUGGED before capture starts."
} else {
  Info "GL310 not plugged in right now."
}

# --- Which USBPcap hub is it on? -------------------------------------------
if ($usbpcapCmd) {
  $ifs = & $usbpcapCmd --extcap-interfaces 2>$null | Select-String -Pattern 'value=(\\\\\.\\USBPcap\d+)' |
         ForEach-Object { $_.Matches[0].Groups[1].Value }
  if ($ifs) {
    Info ("USBPcap interfaces: " + ($ifs -join ', '))
    $found = $false
    foreach ($i in $ifs) {
      $cfg = & $usbpcapCmd --extcap-interface $i --extcap-config 2>$null
      $hit = $cfg | Select-String -Pattern 'AVer|07CA|C835|AVerMedia' -SimpleMatch:$false
      if ($hit) { Ok "GL310 is under $i"; $hit | ForEach-Object { Info $_.Line.Trim() }; $found = $true }
    }
    if (-not $found -and $dev) { Info "Could not match the card to a hub automatically - use Wireshark's gear icon per USBPcap interface." }
  }
}

# --- Power ------------------------------------------------------------------
$bat = Get-CimInstance Win32_Battery
if ($bat) {
  $ac = $bat.BatteryStatus -in 2, 6, 7, 8, 9
  $msg = "battery $($bat.EstimatedChargeRemaining)%$(if ($ac) {' on AC'} else {' ON BATTERY'})"
  if ($ac -or $bat.EstimatedChargeRemaining -ge 60) { Ok $msg } else { Bad "$msg - plug in before capturing" }
}

# --- Output folder ----------------------------------------------------------
$cap = Join-Path (Split-Path $PSScriptRoot) 'captures'
if (Test-Path $cap) { Ok "captures folder: $cap" } else { Info "captures folder will be: $cap" }

Write-Host ""
if ($fail -eq 0) { Write-Host "Ready for Stage 03." -ForegroundColor Green }
else { Write-Host "$fail item(s) to sort out first." -ForegroundColor Yellow }
