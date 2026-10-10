#Requires -RunAsAdministrator
#Requires -PSEdition Desktop
<#
.SYNOPSIS
  Signs and installs the Canon LBP-1120 CAPT (x64) test driver and creates the printer.

.DESCRIPTION
  0. Copies the package into a fresh, randomly named directory under %SystemRoot%\Temp that
     only SYSTEM and Administrators can write, and prints the SHA-256 of each staged file.
     All later steps use those copies, never the user-writable build folder.
  1. Creates a fresh self-signed code-signing certificate and adds its public part, from
     memory, to LocalMachine\Root and LocalMachine\TrustedPublisher (older copies removed).
  2. Builds lbp1120v3.cat from lbp1120v3.cdf with the catalog functions of wintrust.dll (what
     makecat.exe does), signs both DLLs and the catalog (Set-AuthenticodeSignature, SHA-256),
     then DELETES the private key so nothing else can be signed with a machine-trusted cert.
  3. Adds the package to the driver store (pnputil), installs the printer driver and creates
     the printer on the USB port. The INF also installs the "LBP1120 CAPT Monitor" language
     monitor. The staging directory is deleted.

  Windows PowerShell 5.1 (powershell.exe) only; no Windows SDK needed. Run build.cmd first, or
  use a release zip, whose pkg\ folder holds the built package.
  Run uninstall.ps1 before reinstalling a newer build. If System32 still holds another build
  of lbp1120lm.dll, step 4 replaces it (restarting the spooler).
#>
param(
    [string]$PackageDir  = (Join-Path $PSScriptRoot 'pkg'),
    [string]$PrinterName = 'Canon LBP-1120 (CAPT x64)',
    [string]$PortName    = 'USB001',
    [switch]$EnableLog    # sets LogLevel=1 (protocol log in %ProgramData%\LBP1120\lm.log)
)
$ErrorActionPreference = 'Stop'
# Load the Cert: provider from this PowerShell's own modules; a PSModulePath inherited from
# PowerShell 7 would otherwise make Windows PowerShell pick an incompatible copy.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1')

$DriverName = 'Canon LBP-1120 CAPT (x64)'
$Monitor    = 'LBP1120 CAPT Monitor'
$Subject    = 'CN=LBP1120 Test Driver Signing'

foreach ($f in 'lbp1120v3.inf', 'lbp1120v3.cdf', 'lbp1120.gpd', 'lbp1120.ini', 'lbp1120rnd.dll', 'lbp1120lm.dll') {
    if (-not (Test-Path (Join-Path $PackageDir $f))) { throw "Missing $f in $PackageDir - run build.cmd first." }
}
if (Get-Printer -Name $PrinterName -ErrorAction SilentlyContinue) {
    throw "Printer '$PrinterName' already exists - run uninstall.ps1 first."
}
if (-not (Get-PrinterPort -Name $PortName -ErrorAction SilentlyContinue)) {
    throw "Port $PortName not found - is the LBP-1120 plugged in and powered on?"
}

# 0. Stage the package into a fresh, randomly named directory that only SYSTEM and
#    Administrators can write, created atomically with that DACL. Everything below signs and
#    installs these copies, so a non-elevated process cannot swap files between check and use.
$sec = New-Object Security.AccessControl.DirectorySecurity
$sec.SetSecurityDescriptorSddlForm('O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)')
$Stage = Join-Path $env:SystemRoot ('Temp\lbp1120-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $Stage) { throw "Staging directory unexpectedly exists: $Stage" }
[void][IO.Directory]::CreateDirectory($Stage, $sec)
foreach ($f in 'lbp1120v3.inf', 'lbp1120v3.cdf', 'lbp1120.gpd', 'lbp1120.ini', 'lbp1120rnd.dll', 'lbp1120lm.dll') {
    Copy-Item -LiteralPath (Join-Path $PackageDir $f) -Destination $Stage
}
"Staged package in $Stage :"
Get-ChildItem -LiteralPath $Stage | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '  {0}  {1}' -f $_.Hash, (Split-Path $_.Path -Leaf) }

# makecat.exe equivalent: makecat is a thin wrapper around these wintrust.dll functions, which
# every Windows has. The catalog is built from lbp1120v3.cdf exactly as makecat would build it.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace Lbp1120 {
public static class Catalog {
    [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode)]
    delegate void ParseError(uint area, uint localError, string line);
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATCDFOpen(string pwszFilePath, ParseError pfnParseError);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern IntPtr CryptCATCDFEnumCatAttributes(IntPtr pCDF, IntPtr pPrevAttr, ParseError pfnParseError);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern IntPtr CryptCATCDFEnumMembersByCDFTagEx(IntPtr pCDF, IntPtr pwszPrevCDFTag, ParseError pfnParseError,
                                                          ref IntPtr ppMember, bool fContinueOnError, IntPtr pvReserved);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern bool CryptCATCDFClose(IntPtr pCDF);

    static readonly List<string> errors = new List<string>();
    static readonly ParseError onError = (area, err, line) =>
        errors.Add(string.Format("area 0x{0:x8}, error 0x{1:x8}: {2}", area, err, line));

    // Relative names in the CDF (members, ResultDir) resolve against the process current directory.
    public static int Make(string cdfPath) {
        errors.Clear();
        IntPtr cdf = CryptCATCDFOpen(cdfPath, onError);
        if (cdf == IntPtr.Zero)
            throw new InvalidOperationException("CryptCATCDFOpen failed (" + Marshal.GetLastWin32Error() + ") " + string.Join("; ", errors.ToArray()));
        int members = 0;
        IntPtr attr = IntPtr.Zero, tag = IntPtr.Zero, member = IntPtr.Zero;
        try {
            while ((attr = CryptCATCDFEnumCatAttributes(cdf, attr, onError)) != IntPtr.Zero) { }  // [CatalogHeader] CATATTRn (OSAttr)
            while ((tag = CryptCATCDFEnumMembersByCDFTagEx(cdf, tag, onError, ref member, true, IntPtr.Zero)) != IntPtr.Zero)
                members++;
        } finally {
            if (!CryptCATCDFClose(cdf)) errors.Add("CryptCATCDFClose failed (" + Marshal.GetLastWin32Error() + ")");
        }
        if (errors.Count > 0) throw new InvalidOperationException("catalog: " + string.Join("; ", errors.ToArray()));
        return members;
    }
}
}
'@

try {
    # 1-2. Certificate, catalog, signatures. The public certificate goes into the machine stores
    #      straight from memory (no .cer file on disk); the private key never outlives this block.
    Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |
        Where-Object Subject -eq $Subject | Remove-Item
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
            -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable `
            -NotAfter (Get-Date).AddYears(2)
    try {
        $pub = New-Object Security.Cryptography.X509Certificates.X509Certificate2 (, $cert.RawData)
        foreach ($storeName in 'Root', 'TrustedPublisher') {
            $store = New-Object Security.Cryptography.X509Certificates.X509Store ($storeName, 'LocalMachine')
            $store.Open('ReadWrite')
            try { $store.Add($pub) } finally { $store.Close() }
        }

        $cwd = [Environment]::CurrentDirectory
        [Environment]::CurrentDirectory = $Stage
        try {
            $n = [Lbp1120.Catalog]::Make((Join-Path $Stage 'lbp1120v3.cdf'))
            if ($n -ne 5) { throw "catalog: $n members instead of 5" }
            "Catalog lbp1120v3.cat built ($n files)"
            foreach ($f in 'lbp1120rnd.dll', 'lbp1120lm.dll', 'lbp1120v3.cat') {
                [void](Set-AuthenticodeSignature -FilePath (Join-Path $Stage $f) -Certificate $cert -HashAlgorithm SHA256)
                $sig = Get-AuthenticodeSignature -FilePath (Join-Path $Stage $f)
                if ($sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw "Signing $f failed: $($sig.Status) $($sig.StatusMessage)" }
                "Signed $f"
            }
            # Keep the signed monitor in memory: step 4 checks that System32 really got this build.
            $lmBytes = [IO.File]::ReadAllBytes((Join-Path $Stage 'lbp1120lm.dll'))
        } finally { [Environment]::CurrentDirectory = $cwd }
    } finally {
        Remove-Item -Path "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
    }

    # 3. Driver store, printer driver, printer.
    pnputil /add-driver (Join-Path $Stage 'lbp1120v3.inf')
    if ($LASTEXITCODE -notin 0, 259, 3010) { throw "pnputil failed ($LASTEXITCODE)" }
} finally {
    Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue
}
Add-PrinterDriver -Name $DriverName
Add-Printer -Name $PrinterName -DriverName $DriverName -PortName $PortName

# 4. The spooler does not replace a monitor DLL that is already in System32 (e.g. left there,
#    still loaded, by uninstall.ps1), so an update could silently keep running the old monitor.
#    Compare hashes and, if they differ, write the new build with the spooler stopped.
$sha = [Security.Cryptography.SHA256]::Create()
$want = [BitConverter]::ToString($sha.ComputeHash($lmBytes))
$lmDll = Join-Path $env:SystemRoot 'System32\lbp1120lm.dll'
$have = if (Test-Path -LiteralPath $lmDll) { [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($lmDll))) }
if ($have -ne $want) {
    "Replacing $lmDll with the new build (spooler restarted)"
    Stop-Service Spooler -Force
    try { [IO.File]::WriteAllBytes($lmDll, $lmBytes) } finally { Start-Service Spooler }
    if ([BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($lmDll))) -ne $want) { throw "$lmDll does not match the new build" }
}
$lmVer = [regex]::Match([Text.Encoding]::ASCII.GetString($lmBytes), 'lbp1120lm \d+\.\d+').Value

$monKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Print\Monitors\$Monitor"
if ($EnableLog -and (Test-Path $monKey)) {
    Set-ItemProperty -Path $monKey -Name LogLevel -Type DWord -Value 1
}

# Summary
Get-Printer -Name $PrinterName | Format-List Name, DriverName, PortName, PrinterStatus
"Language monitor registered : $(Test-Path $monKey) ($lmVer)"
"Logging                     : $(if ($EnableLog) { 'LogLevel=1 -> ' + $env:ProgramData + '\LBP1120\lm.log' } else { 'off' })"
