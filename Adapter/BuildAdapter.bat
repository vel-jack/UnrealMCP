@echo off
setlocal

set "ADAPTER_ROOT=%~dp0"
set "PROJECT_FILE=%ADAPTER_ROOT%UnrealMCP.Adapter\UnrealMCP.Adapter.csproj"
set "ADAPTER_EXE=%ADAPTER_ROOT%UnrealMCP.Adapter\bin\Release\net9.0-windows\UnrealMCP.Adapter.exe"
echo UnrealMCP Adapter Release Build
echo Project: %PROJECT_FILE%
echo.

where dotnet >nul 2>&1
if errorlevel 1 (
    echo.
    echo ERROR: dotnet was not found on PATH. Install the required .NET SDK and retry.
    echo.
    if /I not "%~1"=="--no-pause" pause
    exit /b 1
)

dotnet build "%PROJECT_FILE%" -c Release
if errorlevel 1 (
    echo.
    echo ERROR: The adapter build failed.
    echo If UnrealMCP.Adapter.exe is locked, close every AI agent or MCP client using this adapter, then run this file again.
    echo.
    if /I not "%~1"=="--no-pause" pause
    exit /b 1
)

if not exist "%ADAPTER_EXE%" (
    echo.
    echo ERROR: The build reported success, but the expected adapter executable was not found:
    echo %ADAPTER_EXE%
    echo.
    if /I not "%~1"=="--no-pause" pause
    exit /b 1
)

echo.
echo Running adapter help smoke check...
"%ADAPTER_EXE%" help >nul
if errorlevel 1 (
    echo.
    echo ERROR: The adapter help smoke check failed.
    echo.
    if /I not "%~1"=="--no-pause" pause
    exit /b 1
)

echo Running adapter protocol doctor...
"%ADAPTER_EXE%" doctor --json
if errorlevel 1 (
    echo.
    echo ERROR: The adapter protocol doctor failed.
    echo.
    if /I not "%~1"=="--no-pause" pause
    exit /b 1
)

echo.
echo UnrealMCP Adapter built and verified successfully.
echo Output: %ADAPTER_EXE%
echo.
if /I not "%~1"=="--no-pause" pause
exit /b 0
