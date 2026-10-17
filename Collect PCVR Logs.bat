@echo off
setlocal
title Collect PCVR support logs
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\collect-pcvr-logs.ps1"
pause
