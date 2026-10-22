# Local n6-platform snapshot

These sources were copied from the sibling `n6-platform` project on 2026-10-22
so that a complete firmware checkout no longer depends on another folder.
The platform builds against this project's `Inc`, `Lib` and `STM32Cube_FW_N6`
directories. Its output backend follows `APP_DISPLAY_OUTPUT` in the parent
CMake project.

To deliberately use a different copy, configure with
`-DN6_PLATFORM_DIR=<path>`. The Windows build script selects this local copy.
