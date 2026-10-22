# Offline Windows firmware build and flashing

The supported portable firmware workflow uses CMake + Ninja + Arm GCC from
STM32CubeCLT. It needs no network access, Python, Node.js, Git, STM32CubeIDE,
VS Code extension, or ST Edge AI installation on the destination PC. Generated
models, the AI runtime, camera middleware and the local `Lib/n6-platform`
sources are included. Model regeneration is a separate, optional workflow.

## Prepare the transfer on this PC

From the project folder in Windows PowerShell or a terminal:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/export-offline.ps1
```

This creates `offline-package/STM32N6`. Copy **that entire folder** to the
offline PC, for example `C:\N6`. It contains the firmware sources, model flash
images, bootloader, scripts, and a complete copy of STM32CubeCLT under
`tools/STM32CubeCLT`. DLLs, external loaders and ST-LINK driver installers
are included; copying only the compiler or programmer EXE is insufficient.
The tool installation adds about 2.3 GiB before compression. Keep the vendor
license files with the tools.

Options:

```powershell
# Explicit tool installation and a new destination (spaces supported).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/export-offline.ps1 -CubeCltRoot 'D:\ST\STM32CubeCLT_1.18.0' -Destination 'D:\USB Transfer\STM32N6'

# Sources only, when the offline machine already has its tool installation.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/export-offline.ps1 -SourceOnly -Destination 'D:\N6-source'
```

The destination must not already exist. Build caches, the viewer and model
generation workspaces are excluded. A SHA-256 manifest records the transferred
files. To check the copy **before editing sources**:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/verify-offline.ps1
```

The checked tools are Windows x64: STM32CubeCLT 1.18.0, GCC 13.3.1,
CMake 3.28.1, Ninja 1.11.1 and CubeProgrammer/signing tool 2.19.0.
Use a supported x64 Windows machine. Windows PowerShell 5.1 is sufficient.
If Windows reports a missing runtime DLL, keep the full copied tool directory
and use the corresponding vendor offline installer/prerequisite installer.
Driver installation requires administrator rights; ordinary builds do not.

## Build on the offline PC

Open a terminal in the copied folder:

```powershell
.\build-windows.cmd -CheckOnly
.\build-windows.cmd

# Other supported configurations:
.\build-windows.cmd -Preset Debug-LCD
.\build-windows.cmd -Preset Release
```

The default is Debug with USB UVC output. The script configures, compiles and
signs the application. Outputs live in `build/<preset>/`:

| File | Purpose |
| --- | --- |
| `Project.elf` | Debug symbols and loading into RAM |
| `Project.bin` | Raw application before signing |
| `Project.hex` | RAM-linked application; **not** the external-flash boot image |
| `Project_sign.bin` | Bootable application to program at `0x70100000` |

Tool discovery checks `-CubeCltRoot`, then the bundled `tools/STM32CubeCLT`,
then `STM32_CUBE_CLT`, then installed `C:\ST\STM32CubeCLT*` folders, then PATH.
It only changes PATH for the script and child processes. A separate
CubeProgrammer installation can be selected with `-ProgrammerRoot` (its root
or `bin` directory) or `STM32_PROGRAMMER_ROOT`.

```powershell
.\build-windows.cmd -CubeCltRoot 'D:\ST\STM32CubeCLT_1.18.0' -Jobs 8
```

Do not transfer `build/` from another machine: CMake caches absolute paths.
The script refreshes the CMake cache when it detects a changed project path.
After changing tool installations, use `-Fresh` to clear cached tool choices
(requires CMake 3.24 or newer).

## Flash the STM32N6570-DK

1. Put the board in **DEV mode** (see [Boot Overview](Boot-Overview.md)).
2. Connect the ST-LINK USB port with a data-capable USB cable.
3. If ST-LINK is absent from Device Manager, install its driver using
   `tools/STM32CubeCLT/drivers/STLinkUSBDriver/stlink_winusb_install.bat`
   as administrator. The complete driver folder must remain beside it.
4. Build the desired preset, then choose the matching flash command:

```powershell
# Display and validate the complete plan without accessing hardware:
.\flash-windows.cmd -All -DryRun

# Initial setup or changed AI models: bootloader, BOTH models, signed app.
.\flash-windows.cmd -All

# Later application updates with the same model weights:
.\flash-windows.cmd

# LCD build, with a particular ST-LINK if multiple boards are connected:
.\flash-windows.cmd -Preset Debug-LCD -SerialNumber YOUR_STLINK_SERIAL
```

Every selected file and the board's external loader are checked before the
first write. Each write is verified, and any failed command stops the script.
The script does not perform a whole-chip erase. First setup writes:

| Image | External flash address |
| --- | --- |
| `Binary/ai_fsbl.hex` | From the HEX records |
| `Model/fd_data.hex` | `0x70380000` (from HEX records) |
| `Model/faceid_data.hex` | `0x72000000` (from HEX records) |
| `build/<preset>/Project_sign.bin` | `0x70100000` |

After successful programming, select **boot from flash** mode and power cycle.
The old `Binary/network_data.hex` and `Binary/x-cube-n6-ai-h264-usb-uvc.hex`
belong to the original demo and are not the current face-recognition build.
They are deliberately excluded from the transfer package.

## VS Code and alternative builds

The supplied Windows VS Code build tasks call the same PowerShell scripts.
Ctrl+Shift+B builds Debug UVC. The debugger configurations use the bundled
CLT SVD and external loader paths. Debugging through the STM32 extension
still requires VS Code and that extension to be installed separately; bring
their offline installers/VSIX files if needed. The command-line workflow does
not require them. The CMake Tools extension's direct commands require CMake,
Ninja, GCC and the signing tool on its own process PATH; use the supplied
tasks for automatic tool discovery.

The older Makefile, IAR and CubeIDE projects do not define this current CMake
build and are not the portable workflow. Regenerating the AI models requires
ST Edge AI and can change runtime/model compatibility. Existing generated
sources and HEX files are sufficient to build and flash offline.

## Common failures

| Symptom | Action |
| --- | --- |
| Missing `../n6-platform` | Use this updated project, including `Lib/n6-platform`. |
| Missing AI header / a reference to `C:\ST\STEdgeAI` | Use this updated CMake configuration and copy the complete `Lib/AI_Runtime`. |
| CMake references the original PC/folder | Run `build-windows.cmd -Fresh`. |
| Tool is not found | Run `-CheckOnly`; pass `-CubeCltRoot` or copy the complete CLT. |
| A tool cannot start / missing DLL | Recopy its full installation; verify the transfer manifest. |
| External loader missing | Keep `STM32CubeProgrammer/bin/ExternalLoader` with the tools. |
| ST-LINK not detected | Install the bundled driver; check cable, power and DEV mode. |
| Board does not boot after programming | Flash with `-All` after building, then select flash boot and power cycle. |
| Paths too long during extraction/build | Use a short destination such as `C:\N6`. |

Firmware compilation and signing can be checked without hardware. Actual
programming, driver installation and boot behavior require the target board.
