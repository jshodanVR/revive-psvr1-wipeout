@echo off
setlocal
title RevivePSVR1 - WipEout
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\launch-psvr-test.ps1" -Menu -Title Wipeout -FovOf headset
if errorlevel 1 pause
