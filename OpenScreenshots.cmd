@echo off
setlocal
set "SCREENSHOT_DIR=D:\MovedFromC\Users\BYND\Desktop\screenshot"
set "QV_SCREENSHOT_DIR=%SCREENSHOT_DIR%"
set "VIEWER=%~dp0out\build\PGO-Native\QuickView.exe"

if not exist "%VIEWER%" (
  echo QuickView.exe not found: "%VIEWER%"
  exit /b 1
)
if not exist "%SCREENSHOT_DIR%\" (
  echo Screenshot directory not found: "%SCREENSHOT_DIR%"
  exit /b 1
)
set "IMAGE="
if not "%~1"=="" if exist "%~1" set "IMAGE=%~f1"
if not defined IMAGE for /f "usebackq delims=" %%I in (`powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$e=[IO.Directory]::EnumerateFiles($env:QV_SCREENSHOT_DIR,'*.avif').GetEnumerator(); if($e.MoveNext()){$e.Current}"`) do if not defined IMAGE set "IMAGE=%%I"
if not defined IMAGE (
  echo No images found in: "%SCREENSHOT_DIR%"
  exit /b 1
)

start "" "%VIEWER%" --viewer-child "%IMAGE%"
