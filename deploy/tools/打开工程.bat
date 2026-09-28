@echo off
chcp 65001 >nul
cd /d "%~dp0"
rem 以"带界面"方式打开工程（制作/检查身份用）。
rem 用法：把 .uproject 拖到本 bat 上，或：打开工程.bat "D:\...\MH_Line01\MH_Line01.uproject"
rem 注意：同一工程不要同时跑无头管线（会共写同一份队列）——先 [暂停] 再打开。

set PROJ=%~1
if "%PROJ%"=="" (
  echo [ERROR] 缺少工程路径。请把 .uproject 文件拖到本 bat 上。
  pause
  exit /b 1
)
if not exist "%PROJ%" (
  echo [ERROR] 工程不存在：%PROJ%
  pause
  exit /b 1
)
echo [打开] %PROJ%
start "" "D:\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "%PROJ%"
echo 已在编辑器中打开（做完身份记得保存 + 关闭编辑器，再回到 GUI 点 [启动 / 继续]）
timeout /t 3 >nul
