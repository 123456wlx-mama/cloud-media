@echo off
setlocal enabledelayedexpansion

rem cloud-media one-click start
rem
rem   start.bat              local docker stack (mysql + redis + server) + client
rem   start.bat vm           connect to the VM server 192.168.142.131 (no docker)
rem   start.bat vm <IP>      connect to the given IP (no docker)
rem
rem   local mode auto-logs in with the demo account (it only exists in the docker DB).
rem   vm mode opens the login dialog, because the demo account is not in the VM DB.

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "CLIENT_EXE=%ROOT%\build-client\bin\cloud-media-client.exe"
set "PORT=8004"

set "HOST=127.0.0.1"
set "SERVE_LOCAL=1"
set "DEMO_ARGS=--tel 13900000000 --password demo123456"

if /i "%~1"=="vm" (
    set "SERVE_LOCAL=0"
    set "HOST=%~2"
    if "!HOST!"=="" set "HOST=192.168.142.131"
    rem ???????? docker ????????????????
    set "DEMO_ARGS="
)

if "!SERVE_LOCAL!"=="1" (
    echo [1/4] Check docker ...
    docker version >nul 2>&1
    if errorlevel 1 (
        echo Docker is not running. Start Docker Desktop, then re-run this script.
        echo To use the VM server instead: start.bat vm 192.168.142.131
        goto :fail
    )
) else (
    echo [1/4] Skip docker ^(mode: vm, host=!HOST!^)
)

echo [2/4] Check client binary ...
if not exist "%CLIENT_EXE%" (
    echo Client binary not found: build-client\bin\cloud-media-client.exe
    echo Build it first. See README, section "5 minutes", or run:
    echo   cmake -S . -B build-client -G "MinGW Makefiles" -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON
    echo Then re-run this script.
    goto :fail
)
if not exist "%ROOT%\build-client\bin\Qt5Core.dll" (
    echo Qt DLLs missing next to the client exe.
    echo Run: cmake --build build-client   -- POST_BUILD deploys Qt DLLs
    goto :fail
)

if "!SERVE_LOCAL!"=="1" (
    echo [3/4] Start server stack ^(mysql + redis + server^) ...
    docker compose up -d --build
    if errorlevel 1 (
        echo docker compose failed. Check logs with: docker compose logs
        goto :fail
    )
) else (
    echo [3/4] Skip docker compose ^(using remote server^)
)

echo [4/4] Wait for server on !HOST!:!PORT! ...
set /a TRIES=0
:wait_server
powershell -NoProfile -Command "$c=New-Object Net.Sockets.TcpClient; try { $c.Connect('!HOST!', %PORT%); exit 0 } catch { exit 1 } finally { $c.Close() }" >nul 2>&1
if errorlevel 1 (
    set /a TRIES+=1
    if !TRIES! GEQ 30 (
        echo Server did not become ready within 60s at !HOST!:!PORT!.
        echo Port !PORT! listeners:
        netstat -ano | findstr ":!PORT!"
        if "!SERVE_LOCAL!"=="1" echo Check logs with: docker compose logs server
        if "!SERVE_LOCAL!"=="0" echo Check the VM: is cloud-media-server running? is the IP correct?
        goto :fail
    )
    ping -n 3 127.0.0.1 >nul
    goto :wait_server
)

echo.
echo Server is ready at !HOST!:!PORT!
if "!SERVE_LOCAL!"=="1" (
    echo Launching client with demo account 13900000000 ...
    echo Server logs: docker compose logs -f server
) else (
    echo Launching client against the VM server; log in with your own account.
)
ver >nul
start "cloud-media client" "%CLIENT_EXE%" --host !HOST! --port %PORT% !DEMO_ARGS!
if errorlevel 1 (
    echo Could not launch the client.
    goto :fail
)
if "!SERVE_LOCAL!"=="1" echo Client launched. Demo account: 13900000000 / demo123456
exit /b 0

:fail
echo.
echo Start failed. Review the message above.
pause
exit /b 1
