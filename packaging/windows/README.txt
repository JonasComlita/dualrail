Ternary OS Desktop Preview
==========================

Run TernaryOS.exe to boot the bundled Ternary OS image.

Included files:
- TernaryOS.exe: SDL2 desktop host
- ternary-os.tboot: immutable boot image
- ternary-os.tdisk: writable sparse disk seed
- SDL2.dll and MinGW runtime DLLs required by the executable
- diagnostics/: default location for exported diagnostic bundles

Controls:
- Text input and mouse input are forwarded to the guest.
- Ctrl+Space pauses or resumes the VM.
- Ctrl+R resets the VM.
- Ctrl+D exports diagnostics to the diagnostics folder.
- Ctrl+Q exits the host runtime.
- F1 toggles debug details in the window title.
- Esc is forwarded to the guest.

Notes:
- The guest disk is mutable. Keep a backup of ternary-os.tdisk if you want to
  preserve a known-good starting state.
- This is a Windows-first preview package. Linux and macOS builds use the same
  runtime layer but are not staged by this package target yet.
