@echo off
set cflags=-Wall -Wextra -Os -s -mwindows -DUNICODE -D_UNICODE -fno-stack-protector -fno-tree-loop-distribute-patterns -fomit-frame-pointer -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -static -nostdlib -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none -Wl,--file-alignment=512 -Wl,-e,WinMainCRTStartup
windres -O coff -i LayoutToggle.rc -o LayoutToggle.res.o || exit /b 1
gcc %cflags% -o LayoutToggle.exe LayoutToggle.c LayoutToggle.res.o -lkernel32 -luser32 -lshell32 -ladvapi32 || exit /b 1
if not exist tests mkdir tests
gcc %cflags% -DTESTING -o tests\LayoutToggle_test.exe LayoutToggle.c LayoutToggle.res.o -lkernel32 -luser32 -lshell32 -ladvapi32 || exit /b 1
del LayoutToggle.res.o
