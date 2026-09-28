@echo off
chcp 65001 >nul
cd /d "%~dp0"
rem 监控面板 GUI（只读监控 + 身份绑定 + 工程切换）
rem 干净机器首跑：没有 pipeline.config.json 时自动初始化

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

set CONFIG=%~dp0pipeline.config.json
set GUI=%~dp0..\ui\src\MhsPipeline.App\bin\Debug\net9.0-windows\MhsPipeline.App.exe
if not exist "%GUI%" (
  echo [ERROR] 未找到 GUI：%GUI%
  echo 请先编译：在 deploy\ui 下执行
  echo   dotnet build src\MhsPipeline.App\MhsPipeline.App.csproj
  pause
  exit /b 1
)
echo [启动] 监控面板 · 配置=%CONFIG%
start "" "%GUI%" "%CONFIG%"
