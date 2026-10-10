#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Removes everything install.ps1 added: printers using the driver, the driver (and its
  driver-store package), the "LBP1120 CAPT Monitor" language monitor, and the test
  certificate from LocalMachine\Root and LocalMachine\TrustedPublisher.
#>
$ErrorActionPreference = 'Stop'
# Load the Cert: provider from this PowerShell's own modules (see install.ps1).
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1')

$DriverName = 'Canon LBP-1120 CAPT (x64)'
$Monitor    = 'LBP1120 CAPT Monitor'
$Subject    = 'CN=LBP1120 Test Driver Signing'

Get-Printer | Where-Object DriverName -eq $DriverName | ForEach-Object {
    "Removing printer $($_.Name)"; Remove-Printer -InputObject $_
}
if (Get-PrinterDriver -Name $DriverName -ErrorAction SilentlyContinue) {
    "Removing driver $DriverName"; Remove-PrinterDriver -Name $DriverName -RemoveFromDriverStore
}

Add-Type -Namespace Lbp1120 -Name Spl -MemberDefinition @'
[DllImport("winspool.drv", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool DeleteMonitorW(string pName, string pEnvironment, string pMonitorName);
'@
$monKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Print\Monitors\$Monitor"
$dll = Join-Path $env:SystemRoot 'System32\lbp1120lm.dll'
if ((Test-Path $monKey) -and [Lbp1120.Spl]::DeleteMonitorW($null, $null, $Monitor)) { "Removed language monitor $Monitor" }
# The spooler keeps a monitor loaded, and DeleteMonitor reports it "in use", until it restarts:
# remove what is left (registry key, DLL) with the spooler stopped.
if ((Test-Path $monKey) -or (Test-Path $dll)) {
    Stop-Service Spooler -Force
    try {
        if (Test-Path $monKey) { Remove-Item $monKey -Recurse; "Removed language monitor $Monitor (spooler stopped)" }
        if (Test-Path $dll) { Remove-Item $dll; "Removed $dll" }
    } finally { Start-Service Spooler }
}

Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |
    Where-Object Subject -eq $Subject | ForEach-Object { "Removing certificate $($_.Thumbprint) from $($_.PSParentPath -replace '.*::', '')"; Remove-Item $_.PSPath }
"Done."
