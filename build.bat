@echo off
rem Builds the Gundam Operation Troy PC recomp from your own copy of the game.
rem See tools\build.ps1 for what it does. A log is written to out\build.log.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1" %*
