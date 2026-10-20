@echo off
setlocal enabledelayedexpansion

rem cloud-media one-click start (demo)
rem Starts the server stack (mysql + redis + server) with docker compose,
rem waits until port 8004 is ready, then launches the Qt client.
rem Requirements: Docker Desktop is running; the client has been built once.

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "CLIENT_EXE=%ROOT%\build-client\bin\cloud-media-client.exe"
set "PORT=8004"

echo [1/4] Check docker ...
docker version >nul 2>&1
if errorlevel 1 (
    echo Docker is not running. Start Docker Desktop, then re-run this script.
    goto :fail
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
    echo Run: cmake --build build-client    (POST_BUILD deploys Qt DLLs)
    goto :fail
)

echo [3/4] Start server stack (mysql + redis + server) ...
docker compose up -d --build
if errorlevel 1 (
    echo docker compose failed. Check logs with: docker compose logs
    goto :fail
)

echo [4/4] Wait for server on port %PORT% ...
set /a TRIES=0
:wait_server
powershell -NoProfile -Command "$c=New-Object Net.Sockets.TcpClient; try { $c.Connect('127.0.0.1', %PORT%); exit 0 } catch { exit 1 } finally { $c.Close() }" >nul 2>&1
if errorlevel 1 (
    set /a TRIES+=1
    if !TRIES! GEQ 60 (
        echo Server did not become ready within 120s.
        echo Port %PORT% listeners:
        netstat -ano | findstr ":%PORT%"
        echo Check logs with: docker compose logs server
        goto :fail
    )
    ping -n 3 127.0.0.1 >nul
    goto :wait_server
)

echo.
echo Server is ready at 127.0.0.1:%PORT%
echo Launching client with demo account 13900000000 ...
echo Server logs: docker compose logs -f server
ver >nul
start "cloud-media client" "%CLIENT_EXE%" --host 127.0.0.1 --port %PORT% --tel 13900000000 --password demo123456
if errorlevel 1 (
    echo Could not launch the client.
    goto :fail
)
echo Client launched. Demo account: 13900000000 / demo123456
exit /b 0

:fail
echo.
echo Start failed. Review the message above.
pause
exit /b 1
