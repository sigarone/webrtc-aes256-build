@echo off
rem Runs the probe, keeps the window open and leaves hevc-probe-report.json next to the exe.
"%~dp0hevc-probe.exe" --pause > nul
