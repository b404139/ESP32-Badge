@echo off
rem Windows build helper: activates ESP-IDF via the official export.bat, then runs idf.py.
rem Usage: build.bat set-target esp32s3 ^&^& build.bat build
rem export.bat refuses to run when MSYSTEM is inherited from Git Bash; clear it.
set "MSYSTEM="
if "%IDF_TOOLS_PATH%"=="" set "IDF_TOOLS_PATH=D:\esp-idf\esp-idf-tools-v5.5.2"
if "%IDF_PATH%"=="" set "IDF_PATH=D:\esp-idf\esp-idf-tools-v5.5.2\frameworks\esp-idf-v5.5.2"
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
idf.py %*