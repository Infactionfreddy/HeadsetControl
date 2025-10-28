@echo off
REM HS80 RGB Test Script for Windows
REM Demonstriert die RGB-Funktionalitaet des Corsair HS80

setlocal

set HEADSETCONTROL=build\headsetcontrol.exe

echo ==========================================
echo   Corsair HS80 RGB Test
echo ==========================================
echo.

REM Check if headsetcontrol exists
if not exist "%HEADSETCONTROL%" (
    echo FEHLER: headsetcontrol.exe nicht gefunden in .\build\
    echo Bitte zuerst kompilieren: cd build ^&^& cmake --build .
    pause
    exit /b 1
)

REM Check if device is connected
echo 1. Suche nach HS80 Headset...
%HEADSETCONTROL% -h 2>NUL | findstr /i "HS80" >NUL
if errorlevel 1 (
    echo WARNUNG: Kein HS80 Headset gefunden!
    echo Fortfahren? (j/n)
    set /p response=
    if /i not "!response!"=="j" (
        exit /b 0
    )
) else (
    echo    + Headset gefunden
)
echo.

REM Test 1: Turn LEDs ON
echo 2. Test: LEDs EINSCHALTEN (Corsair Blau)...
%HEADSETCONTROL% -l 1
if errorlevel 1 (
    echo    x Fehler beim Einschalten
) else (
    echo    + LEDs eingeschaltet
)
timeout /t 3 /nobreak >NUL

REM Test 2: Turn LEDs OFF
echo.
echo 3. Test: LEDs AUSSCHALTEN...
%HEADSETCONTROL% -l 0
if errorlevel 1 (
    echo    x Fehler beim Ausschalten
) else (
    echo    + LEDs ausgeschaltet
)
timeout /t 2 /nobreak >NUL

REM Test 3: Turn back ON
echo.
echo 4. Test: LEDs wieder EINSCHALTEN...
%HEADSETCONTROL% -l 1
if errorlevel 1 (
    echo    x Fehler beim Einschalten
) else (
    echo    + LEDs wieder eingeschaltet
)
timeout /t 2 /nobreak >NUL

REM Test 4: Battery status (only for wireless)
echo.
echo 5. Test: AKKU-STATUS abfragen (nur Wireless)...
%HEADSETCONTROL% -b
if errorlevel 1 (
    echo    i Nicht verfuegbar (normal bei USB-Modellen)
) else (
    echo    + Akku-Status abgerufen
)

echo.
echo ==========================================
echo   Tests abgeschlossen!
echo ==========================================
echo.
echo Weitere Befehle:
echo   Hilfe:        %HEADSETCONTROL% -?
echo   Device Info:  %HEADSETCONTROL% -h
echo   LEDs Ein:     %HEADSETCONTROL% -l 1
echo   LEDs Aus:     %HEADSETCONTROL% -l 0
echo   Akku:         %HEADSETCONTROL% -b
echo.

pause
