@echo off
setlocal

set "ADAPTER_ROOT=%~dp0"
set "PROJECT_FILE=%ADAPTER_ROOT%UnrealMCP.Adapter\UnrealMCP.Adapter.csproj"
set "ADAPTER_EXE=%ADAPTER_ROOT%UnrealMCP.Adapter\bin\Release\net9.0-windows\UnrealMCP.Adapter.exe"
set "DEV_BUILD=0"
set "NO_PAUSE=0"
for %%A in (%*) do (
    if /I "%%~A"=="--dev" set "DEV_BUILD=1"
    if /I "%%~A"=="--no-pause" set "NO_PAUSE=1"
)
if "%DEV_BUILD%"=="1" goto choose_dev_output
goto dev_output_ready
:choose_dev_output
set "DEV_OUTPUT=%ADAPTER_ROOT%.dev-builds\%RANDOM%_%RANDOM%"
if exist "%DEV_OUTPUT%" goto choose_dev_output
set "ADAPTER_EXE=%DEV_OUTPUT%\UnrealMCP.Adapter.exe"
:dev_output_ready
echo UnrealMCP Adapter Release Build
echo Project: %PROJECT_FILE%
echo.

where dotnet >nul 2>&1
if errorlevel 1 (
    echo.
    echo ERROR: dotnet was not found on PATH. Install the required .NET SDK and retry.
    echo.
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

if "%DEV_BUILD%"=="1" (
    dotnet build "%PROJECT_FILE%" -c Release -o "%DEV_OUTPUT%"
) else (
    dotnet build "%PROJECT_FILE%" -c Release
)
if errorlevel 1 (
    echo.
    echo ERROR: The adapter build failed.
    echo If the Release executable is locked, use --dev to build beside running clients.
    echo.
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

if not exist "%ADAPTER_EXE%" (
    echo.
    echo ERROR: The build reported success, but the expected adapter executable was not found:
    echo %ADAPTER_EXE%
    echo.
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

echo.
echo Running adapter help smoke check...
"%ADAPTER_EXE%" help >nul
if errorlevel 1 (
    echo.
    echo ERROR: The adapter help smoke check failed.
    echo.
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

echo Running adapter protocol doctor...
"%ADAPTER_EXE%" doctor --json
if errorlevel 1 (
    echo.
    echo ERROR: The adapter protocol doctor failed.
    echo.
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

echo.
echo UnrealMCP Adapter built and verified successfully.
echo Output: %ADAPTER_EXE%
if "%DEV_BUILD%"=="1" echo Reconnect the MCP client to this executable to load the new version.
echo.
if "%NO_PAUSE%"=="0" pause
exit /b 0
