# Program Current Face-Recognition Firmware

For a checked command-line workflow, see [Offline Windows](Offline-Windows.md).
Build with `build-windows.cmd` before programming the current application.

0. Ensure the board is in dev mode (boot switch in dev mode position).
1. Open STM32CubeProgrammer.
2. Select the Disco board through the "External loaders" tab.
3. ST-Link configuration: set mode to "Hot plug".
4. Connect the board.
5. From the "Erasing & programming" tab, select the `Binary/ai_fsbl.hex` file.
6. Wait for flashing to complete.
7. From the "Erasing & programming" tab, program `Model/fd_data.hex` and then `Model/faceid_data.hex`. Their addresses are encoded in the HEX files.
8. Wait for flashing to complete.
9. From the "Erasing & programming" tab, select `build/Debug/Project_sign.bin` and set the start address to `0x70100000`. Use `build/Debug-LCD/Project_sign.bin` for an LCD build.
10. Wait for flashing to complete.
11. Select boot-from-flash mode and power cycle the board.

Enable verification for each write. Do not substitute `Project.hex`: that
image is linked for RAM, while booting from external flash requires the signed
binary. The old application and `network_data.hex` in `Binary/` belong to the
original demo and are not the current face-recognition build.

![Board Selection](../_htmresc/selectBoard.JPG)
![Flash the Hex file](../_htmresc/flashHex.JPG)

