@echo off

devtools\bin\vpc.exe /checkfiles /tf /define:SOURCESDK +shaders +game /mksln tf2_medallium.sln

if errorlevel 1 (
    echo.
    echo VPC failed with exit code %ERRORLEVEL%.
    pause
)