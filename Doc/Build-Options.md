# Build Options

Some features are enabled using build options or by using `app_config.h`:

- [Camera Orientation](#camera-orientation)
- [Video Output Backend](#video-output-backend)

This documentation explains those features and how to modify them.

## Camera Orientation

Cameras allow flipping the image along two axes.

- `CMW_MIRRORFLIP_MIRROR`: Selfie mode
- `CMW_MIRRORFLIP_FLIP`: Flip upside down
- `CMW_MIRRORFLIP_FLIP_MIRROR`: Flip both axes
- `CMW_MIRRORFLIP_NONE`: Default

1. Open [app_config.h](../Inc/app/app_config.h).

2. Change the `SENSOR_<YOUR_SENSOR_NAME>_FLIP` define:
```c
/* Defines: CMW_MIRRORFLIP_NONE; CMW_MIRRORFLIP_FLIP; CMW_MIRRORFLIP_MIRROR; CMW_MIRRORFLIP_FLIP_MIRROR; */

#define SENSOR_<YOUR_SENSOR_NAME>_FLIP CMW_MIRRORFLIP_NONE
```

## Video Output Backend

The firmware has two mutually exclusive compile-time video outputs:

- `APP_OUTPUT_UVC`: H.264 streaming over USB UVC (the default).
- `APP_OUTPUT_LCD`: ARGB8888 output on the STM32N6570-DK on-board 800x480 LCD.

For CMake builds, select the backend with `APP_DISPLAY_OUTPUT=UVC|LCD`. The
provided `Debug` and `Debug-LCD` presets configure those modes respectively.
Projects using another build system should define exactly one of the two
macros; if neither is defined, `app_config.h` selects UVC for compatibility.
