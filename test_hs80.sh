#!/bin/bash
# HS80 RGB Test Script
# Demonstriert die RGB-Funktionalität des Corsair HS80

HEADSETCONTROL="./build/headsetcontrol"

echo "=========================================="
echo "  Corsair HS80 RGB Test"
echo "=========================================="
echo ""

# Check if headsetcontrol exists
if [ ! -f "$HEADSETCONTROL" ]; then
    echo "FEHLER: headsetcontrol nicht gefunden in ./build/"
    echo "Bitte zuerst kompilieren: cd build && make"
    exit 1
fi

# Check if device is connected
echo "1. Suche nach HS80 Headset..."
$HEADSETCONTROL -h 2>&1 | grep -i "HS80" > /dev/null
if [ $? -ne 0 ]; then
    echo "WARNUNG: Kein HS80 Headset gefunden!"
    echo "Fortfahren? (j/n)"
    read -r response
    if [ "$response" != "j" ]; then
        exit 0
    fi
fi

echo "   ✓ Headset gefunden oder Übersprungen"
echo ""

# Test 1: Turn LEDs ON
echo "2. Test: LEDs EINSCHALTEN (Corsair Blau)..."
$HEADSETCONTROL -l 1
if [ $? -eq 0 ]; then
    echo "   ✓ LEDs eingeschaltet"
else
    echo "   ✗ Fehler beim Einschalten"
fi
sleep 3

# Test 2: Turn LEDs OFF
echo ""
echo "3. Test: LEDs AUSSCHALTEN..."
$HEADSETCONTROL -l 0
if [ $? -eq 0 ]; then
    echo "   ✓ LEDs ausgeschaltet"
else
    echo "   ✗ Fehler beim Ausschalten"
fi
sleep 2

# Test 3: Turn back ON
echo ""
echo "4. Test: LEDs wieder EINSCHALTEN..."
$HEADSETCONTROL -l 1
if [ $? -eq 0 ]; then
    echo "   ✓ LEDs wieder eingeschaltet"
else
    echo "   ✗ Fehler beim Einschalten"
fi
sleep 2

# Test 4: Battery status (only for wireless)
echo ""
echo "5. Test: AKKU-STATUS abfragen (nur Wireless)..."
$HEADSETCONTROL -b
if [ $? -eq 0 ]; then
    echo "   ✓ Akku-Status abgerufen"
else
    echo "   ℹ Nicht verfügbar (normal bei USB-Modellen)"
fi

echo ""
echo "=========================================="
echo "  Tests abgeschlossen!"
echo "=========================================="
echo ""
echo "Weitere Befehle:"
echo "  Hilfe:        $HEADSETCONTROL -?"
echo "  Device Info:  $HEADSETCONTROL -h"
echo "  LEDs Ein:     $HEADSETCONTROL -l 1"
echo "  LEDs Aus:     $HEADSETCONTROL -l 0"
echo "  Akku:         $HEADSETCONTROL -b"
echo ""
