@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo [停止] 写停止哨兵（当前段完成后优雅退出，最长约 35 分钟）
python launcher.py stop --config "%~dp0pipeline.config.json"
echo.
echo 已发出停止请求；可用 查看状态.bat 确认
pause
