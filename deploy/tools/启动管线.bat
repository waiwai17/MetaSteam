@echo off
chcp 65001 >nul
cd /d "%~dp0"
rem 无头管线：拉起引擎（窗口移出屏幕）+ watchdog（崩溃自愈）
rem 干净机器首跑：没有 pipeline.config.json 时自动初始化（探测 UE → 建工程 → 建 inbox/out）

setlocal
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
  where python >nul 2>nul && set "PY=python"
)
if not defined PY (
  echo [ERROR] 未找到 Python（需要 Python 3.9+）。请安装并勾选 "Add to PATH"。
  pause
  exit /b 1
)

if not exist "%~dp0pipeline.config.json" (
  echo [INIT] 首次运行：正在初始化（探测 UE 并创建工程）...
  %PY% launcher.py init --config "%~dp0pipeline.config.json"
  if errorlevel 1 (
    echo [ERROR] 初始化失败，请按上方提示处理。
    pause
    exit /b 1
  )
)

echo [启动] 无头管线 · 配置=%~dp0pipeline.config.json
%PY% launcher.py start --config "%~dp0pipeline.config.json"
echo.
echo 已退出。查看状态：查看状态.bat  停止：停止管线.bat
pause
