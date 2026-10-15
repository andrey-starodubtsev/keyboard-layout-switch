# Keyboard Layout Switch

A tiny Windows tray app (single C file, no CRT) that makes **Alt+Shift** switch keyboard layouts in most-recently-used order.

## Behavior

- **First press** of Alt+Shift switches to the previously used layout.
- **Pressing Shift again** while Alt is held walks the system layout list. The second press skips the layout the chord started from, so you reach the third layout; after that all layouts are cycled.
- **Direction** matches native Windows: Left Shift goes forward, Right Shift goes backward.
- **Holding Alt** for a moment opens the standard Windows layout flyout (the app sends Win+Space on your behalf) and steers its highlight to the selected layout.
- Alt+Shift combined with any other key (e.g. Alt+Shift+Tab) is left alone.

Nothing is persisted. The low-level keyboard hook is the only "modification"; exiting the app restores default Windows behavior.

## Usage

Run `LayoutToggle.exe`. It lives in the tray (right-click the icon → **Exit**). Only one instance runs at a time.

For the native hotkey not to conflict, you may want to disable it in Windows Settings → Time & language → Typing → Advanced keyboard settings → Input language hot keys.

## Build

Requires GCC (MinGW-w64) on `PATH`:

```
build.cmd
```

This produces `LayoutToggle.exe` and a test build `tests\LayoutToggle_test.exe` (compiled with `-DTESTING`, which accepts the test harness's marked synthetic keys). A `CMakeLists.txt` is also provided.

## Tests

The tests inject keystrokes into a focused test window and observe layout changes, so they need an interactive Windows session with multiple keyboard layouts installed:

```
python tests\test_layouts.py
```

## License

[MIT](LICENSE)
