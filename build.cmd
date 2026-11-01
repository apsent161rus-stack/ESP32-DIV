@echo off
rem ESP32-DIV fork — one-command build/flash helper
setlocal
set "AC=C:\Program Files\Arduino CLI\arduino-cli.exe"
set "FQBN=esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc"
set "PORT=%~2"
if "%PORT%"=="" set "PORT=COM4"
cd /d "%~dp0"

if /i "%~1"=="build" goto build
if /i "%~1"=="flash" goto flash
if /i "%~1"=="monitor" goto monitor
if /i "%~1"=="clean" goto clean
echo usage: build.cmd [build^|flash^|monitor^|clean] [COMx]
exit /b 1

:clean
echo [clean] removing Arduino build cache...
if exist "%LOCALAPPDATA%\arduino\sketches" rmdir /s /q "%LOCALAPPDATA%\arduino\sketches"
if exist "build" rmdir /s /q "build"
exit /b 0

:build
call :clean
echo [build] %FQBN%
"%AC%" compile -b "%FQBN%" --output-dir build ESP32-DIV
exit /b %ERRORLEVEL%

:flash
if not exist "build\ESP32-DIV.ino.bin" (
  echo [flash] no build found, building first...
  call :build
)
echo [flash] port %PORT%
python -m esptool --port %PORT% write-flash 0x0 "build\ESP32-DIV.ino.bootloader.bin" 0x8000 "build\ESP32-DIV.ino.partitions.bin" 0x10000 "build\ESP32-DIV.ino.bin"
exit /b %ERRORLEVEL%

:monitor
"%AC%" monitor -p %PORT% -c baudrate=115200
exit /b %ERRORLEVEL%
