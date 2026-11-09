# Microsoft Windows 1.01 DOS startup compatibility

The supplied Windows 1.01 payload is installed under `C:\WINDOWS1` so its
system files stay together. `WIN100.OVL` in this payload originally contained
an absolute `C:\WINTEST\WIN100.OVL` self-path from the image it was extracted
from; the staged copy is patched to `C:\WINDOWS1\WIN100.OVL`.

The `C:\DOS\WIN.BAT` launcher changes the DOS current directory to `\WINDOWS1`,
starts `WIN.COM`, and restores the root directory after Windows exits. Start
Windows from the root with:

```dos
C:\>WIN
```

You can also start it manually with `CD \WINDOWS1` followed by `WIN.COM`.
`WIN.COM` locates `WIN100.BIN` relative to the current DOS directory, so do not
run the binary while the current directory is `C:\`.

`WIN.COM` launches `WIN100.BIN` with DOS `INT 21h/AX=4B00h` (the supplied
`WIN.COM` binary was checked for this call). The child DOS VM reports DOS 3.34
to `WIN100.BIN`, matching the SETVER entry documented for Windows 1.x on DOS
5 and later. Other DOS programs continue to see DOS 5.00, and
`INT 21h/AX=3306h` reports the true version. The MZ/NE `WIN100.BIN` is accepted
for its DOS-mode MZ startup stub; this does not add a general NE executable
loader to Svindows.

The current compatibility work covers the startup BIOS path exercised by this
configuration: CGA graphics modes 04h/05h/06h, EGA attribute-palette BIOS
services (`INT 10h/AH=10h`), CGA palette selection (`AH=0Bh`), pixel access
(`AH=0Ch/0Dh`), EGA display/configuration queries (`AH=1Ah` and `AH=12h,
BL=10h`), and basic equipment/conventional-memory queries (`INT 11h/12h`).
The CGA 640x200 monochrome mode 06h framebuffer is read from the interleaved
`B800:0000` aperture and presented through the Svindows framebuffer.

Windows 1.01 now boots to MS-DOS Executive in QEMU, with the files in
`C:\WINDOWS1` visible in its window. This was checked against the actual
bundled binaries and framebuffer screenshots. Sending Down and Enter through
QEMU's PS/2 controller selects `CALC.EXE` and opens Calculator successfully.
The saved evidence is in `build/gui/checks/windows101-{desktop,calculator}.png`
and `build/gui/checks/windows101-qemu.log`.

The startup fixes include sequenced relative-JMP displacement fetches, DOS MZ
`/HIGH` placement, PSP creation/switching (INT 21h/50h, 51h, 55h, 62h), FCB
first/next searches (11h/12h), DOS idle notification (INT 28h), the BIOS timer
chain, BIOS keyboard buffer and shift-status query, and idle COM-port status during the
serial-mouse probe. EXEC inherits the display and BIOS data. EGA modes 0Dh-12h
now have four-plane memory, VGA latches, read compare, write modes 0-3, raster
operations, bit masks and framebuffer presentation. Video mode changes update
the BIOS data area as well: Windows' keyboard hook checks `0040:0049` to decide
whether it should process input itself or delegate to the BIOS.

`WIN100.BIN` sets both MZ allocation fields to zero. The DOS loader must give
it the conventional-memory block and load the image at the block's upper end;
loading it immediately above the PSP leaves its private heap unusable. The
behavior follows [Microsoft's EXEC source](https://github.com/microsoft/MS-DOS/blob/main/v4.0/src/DOS/EXEC.ASM).
FCB search results follow the drive-byte/directory-entry layout in the
[MS-DOS 3.3 reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/).

Run `make dos-vm-check` for interpreter, loader/PSP, FCB search, serial-port,
BIOS timer, EGA/CGA, system-data, mouse-cursor and x87 regression checks.
Compatibility still has limits: DPMI protected-mode mouse callback delivery
is not implemented, and DOS processes within one VM share the host-side
file-handle table.

The DOS virtual machine now exposes a Microsoft Mouse-compatible `INT 33h`
driver backed by the host PS/2 pointer. It supports position/buttons, ranges,
press/release counts, mickey ratios, standard and large graphics cursors,
primary and modifier-sensitive callbacks, state save/restore, sensitivity,
display-page state, and driver enable/disable. COM1/COM2 also provide the
three-byte Microsoft serial-mouse protocol used by Windows 1.x drivers. The
framebuffer presenter composites the classic 16x16 arrow or guest-defined
AND/XOR cursor after painting CGA, EGA and VGA frames, honoring the cursor
hotspot and hide state without writing cursor pixels into guest video memory.
For `WIN100.BIN`, the presenter does not draw or overlay any cursor. Windows
must load its own `MOUSE.DRV` and render its pointer into guest video memory;
COM1 only supplies the Microsoft serial-mouse hardware interface. The current
supplied `user/d/windows1` tree contains `MOUSE.COM` and `MOUSE.SYS` (DOS
mouse drivers), but currently lacks the Windows 1.01 `MOUSE.DRV` graphics
driver. Add the correct `MOUSE.DRV` selected for the serial mouse from the
original Windows 1.01 installation media to `user/d/windows1`; the Makefile's
Windows file wildcard will then stage it automatically. DPMI protected-mode
mouse callback delivery is not implemented.


The DOS BIOS Data Area now advertises COM1 (`03F8h`) and COM2 (`02F8h`) and reports both in the INT 11h equipment word, matching the virtual UARTs.
