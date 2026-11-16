# Henge driver disc: install the virtio drivers, unattended.
# Runs from FirstLogonCommands (see ANSWER\UNATTEND.XML) or by double-clicking
# INSTALL.BAT. Everything is logged to C:\Windows\Temp\henge-drivers.log.
$ErrorActionPreference = 'Continue'
$log = 'C:\Windows\Temp\henge-drivers.log'

function Log([string]$m) {
    $line = "{0} {1}" -f (Get-Date).ToString('s'), $m
    Add-Content -LiteralPath $log -Value $line -Encoding utf8
}

Log 'henge driver install: start'

# Find our own disc by volume label; the drive letter is whatever Windows gave it.
$disc = Get-CimInstance Win32_CDROMDrive | Where-Object { $_.VolumeName -eq 'HENGEDRV' } | Select-Object -First 1
if (-not $disc) {
    Log 'no HENGEDRV disc in any drive; aborting'
    exit 1
}
$root = $disc.DriveLetter
Log "disc at $root"

# 1. Trust Red Hat's CA. The virtio drivers are signed by it; without both stores
#    Windows refuses them, and on ARM64 it refuses quietly.
Get-ChildItem -LiteralPath "$root\CERT" -Filter *.cer -ErrorAction SilentlyContinue | ForEach-Object {
    & certutil.exe -addstore -f Root $_.FullName | Out-Null
    & certutil.exe -addstore -f TrustedPublisher $_.FullName | Out-Null
    Log "cert: $($_.Name) -> Root, TrustedPublisher"
}

# 2. Every INF on the disc, in place. pnputil copies the package into the driver
#    store and binds it to whatever hardware matches.
$infs = Get-ChildItem -LiteralPath "$root\DRIVERS" -Recurse -Filter *.inf -ErrorAction SilentlyContinue
foreach ($inf in $infs) {
    Log "inf: $($inf.FullName)"
    $out = & pnputil.exe /add-driver $inf.FullName /install 2>&1
    Log ("pnputil: " + ($out -join ' | '))
}

Log 'henge driver install: done'
exit 0
