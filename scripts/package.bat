@echo off
rem Build and package RigEditor (portable zip, plus setup.exe when Inno Setup 6 is installed).
rem Usage: scripts\package.bat [-SkipBuild]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" %*
