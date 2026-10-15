@echo off
gcc -Wall -Os -s -mwindows -o LayoutToggle.exe LayoutToggle.c -luser32 -lshell32 -ladvapi32 || exit /b 1
if not exist tests mkdir tests
gcc -Wall -Os -s -mwindows -DTESTING -o tests\LayoutToggle_test.exe LayoutToggle.c -luser32 -lshell32 -ladvapi32 || exit /b 1
