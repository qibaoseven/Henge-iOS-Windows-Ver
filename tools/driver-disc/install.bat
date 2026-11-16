@echo off
rem Double-click entry point for the driver disc. FirstLogonCommands runs
rem INSTALL.PS1 directly; this exists for the manual fallback ("does nothing
rem happens, click the disc").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0INSTALL.PS1"
pause
