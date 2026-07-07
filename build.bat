@echo off
REM ============================================================
REM  build.bat - Build audior with MSVC (Developer Command Prompt)
REM ============================================================
setlocal

echo Building audior...

cl.exe /nologo /W4 /TC /O2 ^
    audior.c recorder.c capture.c device.c writer.c audio_convert.c ring_buffer.c guids.c ^
    /link ^
    ole32.lib ^
    mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ^
    /out:audior.exe

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build FAILED.
    exit /b %ERRORLEVEL%
)

echo.
echo Build SUCCESS: audior.exe
endlocal
