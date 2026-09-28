@echo off
chcp 65001 >nul
cd /d "%~dp0"
python launcher.py status --config "%~dp0pipeline.config.json"
echo.
echo 提示：Web 监控 http://127.0.0.1:8902 （手机用 http://^<本机IP^>:8902）
pause
