@echo off
set cflags=-Wall -Wextra -Os -s -mwindows -DUNICODE -D_UNICODE -fno-stack-protector -fno-tree-loop-distribute-patterns -fomit-frame-pointer -static -nostdlib -Wl,--gc-sections -Wl,--strip-all -Wl,-e,WinMainCRTStartup
gcc %cflags% -o LayoutToggle.exe LayoutToggle.c -lkernel32 -luser32 -lshell32 -ladvapi32 || exit /b 1
if not exist tests mkdir tests
gcc %cflags% -DTESTING -o tests\LayoutToggle_test.exe LayoutToggle.c -lkernel32 -luser32 -lshell32 -ladvapi32 || exit /b 1
