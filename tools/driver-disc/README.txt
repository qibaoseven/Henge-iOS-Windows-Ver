HENGE — driver disc
===================

This is the little CD the app mounts next to your install media. It holds the
ARM64 drivers for the machine's devices and the answer file that installs them
without anybody clicking anything.

What is on it
-------------
  HENGE.TAG        the marker the installer looks for (so it never guesses a
                   drive letter)
  INSTALL.PS1      imports Red Hat's code-signing certificate, then pnputil's
                   every INF on this disc
  INSTALL.BAT      double-click fallback for INSTALL.PS1
  ANSWER\UNATTEND.XML  the answer file WinNTSetup wants at deploy time; its
                   FirstLogonCommands runs INSTALL.PS1 at the first logon
  CERT\RHCA.CER    Virtio_Win_Red_Hat_CA.cer, renamed for 8.3
  DRIVERS\VIOGPUDO\  virtio-gpu display driver (viogpudo.sys/.inf/.cat)
  DRIVERS\NETKVM\    virtio-net adapter (netkvm.sys/.inf/.cat)

How it is used
--------------
Stage 1 (WinPE): the disc is already in a tray. When you run WinNTSetup,
point its "unattended" field at ANSWER\UNATTEND.XML on this disc. WinNTSetup
copies it into the target machine.

Stage 3 (first boot of the installed system): the disc is in a tray again,
Windows logs in, FirstLogonCommands finds HENGE.TAG and runs INSTALL.PS1.
Then reboot once.

Manual fallback: open the disc in Explorer and double-click INSTALL.BAT.

The signing caveat, honestly
----------------------------
The virtio drivers are signed by Red Hat's CA, not by Microsoft. Importing that
CA into Root and TrustedPublisher (which INSTALL.PS1 does) is enough on x64.
On Windows 11 ARM64, driver signature enforcement is stricter; if pnputil refuses
the package, the log at C:\Windows\Temp\henge-drivers.log will say so, and the
remaining options are test-signing (needs Secure Boot off) or an inbox driver
that already matches ARM64. Do not assume this step works until it has been
watched working once on real hardware.
