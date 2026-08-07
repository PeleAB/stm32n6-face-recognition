/**
 ******************************************************************************
 * @file    app_display.c
 * @author  GPM Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2024 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#include "svc/app_display.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "app/app.h"
#include "app/app_config.h"
#include "app/app_pipeline.h"
#include "app_postprocess.h"
#include "semphr.h"
#include "stm32n6570_discovery.h"
#include "stm32n6xx_hal.h"
#include "svc/app_stats.h"
#include "svc/app_trackobject.h"
#include "svc/draw.h"
#include "svc/face_detect.h"
#include "svc/figs.h"
#if defined(APP_OUTPUT_UVC)
#include "sysobj_encoder.h"
#include "uvcl.h"
#else
#include "stm32n6570_discovery_lcd.h"
#include "stm32n6570_discovery_bus.h"
#endif
#include "task.h"
#include "utils.h"

#define DBG_INFO_FONT font_12
#define CONF_LEVEL_FONT font_16
#define INF_INFO_FONT font_16
#define OBJ_RECT_COLOR   0xffffffff
#define ROTATED_RECT_COLOR 0xff00d8ff
#define BOX_PADDING_PERCENT 12
#define ROTATED_CORNER_FRACTION 0.24f
#define ROTATED_CORNER_POINTS   7
#define ROTATED_POINT_SIZE      5
#define EYE_LANDMARK_COLOR      0xff00ff00
#define NOSE_LANDMARK_COLOR     0xffffff00
#define MOUTH_LANDMARK_COLOR    0xffff4fff
#define LANDMARK_DOT_SZ         7
#if defined(APP_OUTPUT_UVC)
#define VENC_OUT_BUFFER_SIZE (255 * 1024)
#endif

typedef struct {
  float conf;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
} box_t;

static DRAW_Font_t font_12;
static DRAW_Font_t font_16;
static SemaphoreHandle_t dma2d_lock;
static StaticSemaphore_t dma2d_lock_buffer;
static SemaphoreHandle_t dma2d_sem;
static StaticSemaphore_t dma2d_sem_buffer;

#if defined(APP_OUTPUT_UVC)
static struct uvcl_callbacks uvcl_cbs;
static int uvc_is_active;
static volatile int buffer_flying;
static int force_intra;

static uint8_t venc_out_buffer[VENC_OUT_BUFFER_SIZE] ALIGN_32 UNCACHED;
#else
#define LCD_FRAME_BUFFER_COUNT 2
#define GT911_ADDRESS_PRIMARY  0xBAU
#define GT911_ADDRESS_SECONDARY 0x28U
#define GT911_STATUS_REG       0x814EU
#define GT911_POINT1_REG       0x8150U
#define UI_COLOR_PANEL         0xff101820U
#define UI_COLOR_PANEL_ALT     0xff1b2735U
#define UI_COLOR_PRIMARY       0xff1688d4U
#define UI_COLOR_DANGER        0xffc43d4bU
#define UI_COLOR_DISABLED      0xff4a5563U
#define UI_COLOR_TEXT          0xffffffffU
#define UI_COLOR_MUTED         0xffa9b6c5U

typedef enum {
  LCD_UI_CAMERA = 0,
  LCD_UI_GALLERY,
  LCD_UI_KEYBOARD,
  LCD_UI_ENROLLING,
  LCD_UI_CONFIRM_DELETE,
  LCD_UI_CONFIRM_CLEAR,
} lcd_ui_page_t;

static int lcd_is_active;
static unsigned int lcd_write_index;
static uint8_t lcd_frame_buffer[LCD_FRAME_BUFFER_COUNT]
                               [LCD_DISPLAY_WIDTH * LCD_DISPLAY_HEIGHT * CAPTURE_BPP]
                               ALIGN_32 IN_PSRAM;
static int lcd_touch_available;
static int lcd_touch_latched;
static uint16_t lcd_touch_address;
static lcd_ui_page_t lcd_ui_page;
static uint8_t lcd_ui_delete_slot;
static char lcd_ui_name[FACE_GALLERY_NAME_MAX + 1];
static uint8_t lcd_ui_name_len;
static char lcd_ui_message[40];
#endif

#if defined(APP_OUTPUT_LCD)
static int point_in(int px, int py, int x, int y, int w, int h)
{
  return px >= x && px < x + w && py >= y && py < y + h;
}

static void ui_message(const char *message)
{
  strncpy(lcd_ui_message, message, sizeof(lcd_ui_message) - 1U);
  lcd_ui_message[sizeof(lcd_ui_message) - 1U] = '\0';
}

static void draw_button(uint8_t *buffer, int x, int y, int w, int h,
                        const char *label, uint32_t color, int enabled)
{
  uint32_t fill = enabled ? color : UI_COLOR_DISABLED;
  int text_w = (int)strlen(label) * CONF_LEVEL_FONT.width;
  int text_x = x + MAX(6, (w - text_w) / 2);
  int text_y = y + (h - CONF_LEVEL_FONT.height) / 2;

  DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, x, y, w, h, fill);
  DRAW_RectArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, x, y, w, h,
                  UI_COLOR_TEXT);
  DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    text_x, text_y, "%s", label);
}

static void lcd_touch_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOQ_CLK_ENABLE();

  /* Select the GT911 0x5d address: INT low while reset is released. The LCD
   * and touch controller share PE1 reset, so perform this before LCD init. */
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Pin = GPIO_PIN_4;
  HAL_GPIO_Init(GPIOQ, &gpio);
  HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_4, GPIO_PIN_RESET);

  gpio.Pin = GPIO_PIN_1;
  HAL_GPIO_Init(GPIOE, &gpio);
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_RESET);
  HAL_Delay(12);
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_SET);
  HAL_Delay(8);

  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_NOPULL;
  gpio.Pin = GPIO_PIN_4;
  HAL_GPIO_Init(GPIOQ, &gpio);
  HAL_Delay(50);

  if (BSP_I2C2_Init() != BSP_ERROR_NONE)
    return;
  if (BSP_I2C2_IsReady(GT911_ADDRESS_PRIMARY, 3) == BSP_ERROR_NONE) {
    lcd_touch_address = GT911_ADDRESS_PRIMARY;
    lcd_touch_available = 1;
  } else if (BSP_I2C2_IsReady(GT911_ADDRESS_SECONDARY, 3) == BSP_ERROR_NONE) {
    lcd_touch_address = GT911_ADDRESS_SECONDARY;
    lcd_touch_available = 1;
  }
}

static int lcd_touch_get_tap(int *x, int *y)
{
  uint8_t status;
  uint8_t point[4];
  uint8_t clear = 0;
  uint32_t raw_x;
  uint32_t raw_y;

  if (!lcd_touch_available)
    return 0;
  if (BSP_I2C2_ReadReg16(lcd_touch_address, GT911_STATUS_REG, &status, 1) !=
      BSP_ERROR_NONE)
    return 0;
  if ((status & 0x80U) == 0U)
    return 0;

  if ((status & 0x0fU) == 0U) {
    lcd_touch_latched = 0;
    (void)BSP_I2C2_WriteReg16(lcd_touch_address, GT911_STATUS_REG, &clear, 1);
    return 0;
  }

  if (BSP_I2C2_ReadReg16(lcd_touch_address, GT911_POINT1_REG, point,
                         sizeof(point)) != BSP_ERROR_NONE)
    return 0;
  (void)BSP_I2C2_WriteReg16(lcd_touch_address, GT911_STATUS_REG, &clear, 1);

  if (lcd_touch_latched)
    return 0;
  lcd_touch_latched = 1;
  raw_x = (uint32_t)point[0] | ((uint32_t)point[1] << 8);
  raw_y = (uint32_t)point[2] | ((uint32_t)point[3] << 8);
  *x = (int)MIN(raw_x, LCD_DISPLAY_WIDTH - 1U);
  *y = (int)MIN(raw_y, LCD_DISPLAY_HEIGHT - 1U);
  return 1;
}

static const char *gallery_error(face_gallery_status_t status)
{
  switch (status) {
  case FACE_GALLERY_OK: return "Saved";
  case FACE_GALLERY_ERR_NAME: return "Invalid name";
  case FACE_GALLERY_ERR_BUSY: return "Enrollment busy";
  case FACE_GALLERY_ERR_SAMPLES: return "Need more face samples";
  case FACE_GALLERY_ERR_FULL: return "Gallery is full";
  case FACE_GALLERY_ERR_FLASH: return "Flash write failed";
  case FACE_GALLERY_ERR_SLOT: return "Entry no longer exists";
  default: return "Gallery error";
  }
}

static void lcd_ui_handle_gallery_tap(int x, int y)
{
  int row_y = 142;

  if (point_in(x, y, 650, 42, 90, 42)) {
    lcd_ui_page = LCD_UI_CAMERA;
    return;
  }
  if (FaceGallery_Count() < FACE_GALLERY_MAX_ENTRIES &&
      point_in(x, y, 70, 90, 190, 42)) {
    memset(lcd_ui_name, 0, sizeof(lcd_ui_name));
    lcd_ui_name_len = 0;
    ui_message("");
    lcd_ui_page = LCD_UI_KEYBOARD;
    return;
  }
  if (FaceGallery_Count() > 0 && point_in(x, y, 280, 90, 190, 42)) {
    lcd_ui_page = LCD_UI_CONFIRM_CLEAR;
    return;
  }

  for (uint8_t slot = 0; slot < FACE_GALLERY_MAX_ENTRIES; slot++) {
    char name[FACE_GALLERY_NAME_MAX + 1];
    if (FaceGallery_GetName(slot, name, sizeof(name)) != 0)
      continue;
    if (point_in(x, y, 600, row_y, 120, 34)) {
      lcd_ui_delete_slot = slot;
      lcd_ui_page = LCD_UI_CONFIRM_DELETE;
      return;
    }
    row_y += 38;
  }
}

static void lcd_ui_handle_keyboard_tap(int x, int y)
{
  static const char *rows[] = {
    "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789-_."
  };
  static const int row_y[] = {120, 178, 236, 294};

  for (unsigned int row = 0; row < ARRAY_NB(rows); row++) {
    int count = (int)strlen(rows[row]);
    int start_x = (LCD_DISPLAY_WIDTH - count * 62) / 2;
    for (int col = 0; col < count; col++) {
      if (point_in(x, y, start_x + col * 62, row_y[row], 56, 48)) {
        if (lcd_ui_name_len < FACE_GALLERY_NAME_MAX) {
          lcd_ui_name[lcd_ui_name_len++] = rows[row][col];
          lcd_ui_name[lcd_ui_name_len] = '\0';
        }
        return;
      }
    }
  }

  if (point_in(x, y, 70, 400, 150, 46)) {
    lcd_ui_page = LCD_UI_GALLERY;
  } else if (point_in(x, y, 250, 400, 150, 46)) {
    if (lcd_ui_name_len > 0)
      lcd_ui_name[--lcd_ui_name_len] = '\0';
  } else if (point_in(x, y, 570, 400, 160, 46) && lcd_ui_name_len > 0) {
    face_gallery_status_t st = FaceGallery_StartEnrollment(lcd_ui_name,
                                                            lcd_ui_name_len);
    ui_message(st == FACE_GALLERY_OK ? "Look at the camera alone" :
                                      gallery_error(st));
    if (st == FACE_GALLERY_OK)
      lcd_ui_page = LCD_UI_ENROLLING;
  }
}

static void lcd_ui_handle_tap(int x, int y)
{
  face_gallery_enroll_status_t status;
  face_gallery_status_t result;

  switch (lcd_ui_page) {
  case LCD_UI_CAMERA:
    if (point_in(x, y, 650, 16, 134, 44))
      lcd_ui_page = LCD_UI_GALLERY;
    break;
  case LCD_UI_GALLERY:
    lcd_ui_handle_gallery_tap(x, y);
    break;
  case LCD_UI_KEYBOARD:
    lcd_ui_handle_keyboard_tap(x, y);
    break;
  case LCD_UI_ENROLLING:
    FaceGallery_GetStatus(&status);
    if (point_in(x, y, 190, 330, 180, 48)) {
      app_pipeline_gallery_cancel();
      ui_message("Enrollment canceled");
      lcd_ui_page = LCD_UI_GALLERY;
    } else if (status.samples >= FACE_GALLERY_MIN_SAMPLES &&
               point_in(x, y, 430, 330, 180, 48)) {
      result = app_pipeline_gallery_commit();
      ui_message(gallery_error(result));
      if (result == FACE_GALLERY_OK)
        lcd_ui_page = LCD_UI_GALLERY;
    }
    break;
  case LCD_UI_CONFIRM_DELETE:
    if (point_in(x, y, 210, 300, 170, 48)) {
      lcd_ui_page = LCD_UI_GALLERY;
    } else if (point_in(x, y, 420, 300, 170, 48)) {
      result = app_pipeline_gallery_delete(lcd_ui_delete_slot);
      ui_message(result == FACE_GALLERY_OK ? "Entry deleted" :
                                            gallery_error(result));
      lcd_ui_page = LCD_UI_GALLERY;
    }
    break;
  case LCD_UI_CONFIRM_CLEAR:
    if (point_in(x, y, 210, 300, 170, 48)) {
      lcd_ui_page = LCD_UI_GALLERY;
    } else if (point_in(x, y, 420, 300, 170, 48)) {
      result = app_pipeline_gallery_clear();
      ui_message(result == FACE_GALLERY_OK ? "Gallery cleared" :
                                            gallery_error(result));
      lcd_ui_page = LCD_UI_GALLERY;
    }
    break;
  }
}

static void lcd_ui_draw_gallery(uint8_t *buffer)
{
  int row_y = 142;

  DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 40, 28, 720, 428,
                  UI_COLOR_PANEL);
  DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    70, 52, "Enrollment Gallery (%u/%u)",
                    (unsigned int)FaceGallery_Count(),
                    (unsigned int)FACE_GALLERY_MAX_ENTRIES);
  draw_button(buffer, 650, 42, 90, 42, "CLOSE", UI_COLOR_PANEL_ALT, 1);
  draw_button(buffer, 70, 90, 190, 42, "NEW PERSON", UI_COLOR_PRIMARY,
              FaceGallery_Count() < FACE_GALLERY_MAX_ENTRIES);
  draw_button(buffer, 280, 90, 190, 42, "CLEAR ALL", UI_COLOR_DANGER,
              FaceGallery_Count() > 0);

  for (uint8_t slot = 0; slot < FACE_GALLERY_MAX_ENTRIES; slot++) {
    char name[FACE_GALLERY_NAME_MAX + 1];
    if (FaceGallery_GetName(slot, name, sizeof(name)) != 0)
      continue;
    DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 70, row_y, 650, 34,
                    UI_COLOR_PANEL_ALT);
    DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                      84, row_y + 8, "%s  [slot %u]", name,
                      (unsigned int)slot);
    draw_button(buffer, 600, row_y, 120, 34, "DELETE", UI_COLOR_DANGER, 1);
    row_y += 38;
  }
  if (FaceGallery_Count() == 0)
    DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                      70, 164, "No enrolled identities");
  if (lcd_ui_message[0] != '\0')
    DRAW_PrintfArgbHw(&DBG_INFO_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                      70, 430, "%s", lcd_ui_message);
  if (!lcd_touch_available)
    DRAW_PrintfArgbHw(&DBG_INFO_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                      500, 430, "Touch controller unavailable");
}

static void lcd_ui_draw_keyboard(uint8_t *buffer)
{
  static const char *rows[] = {
    "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789-_."
  };
  static const int row_y[] = {120, 178, 236, 294};
  char key[2] = {0, 0};

  DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 40, 28, 720, 428,
                  UI_COLOR_PANEL);
  DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    70, 48, "Enter person name (max %u)",
                    (unsigned int)FACE_GALLERY_NAME_MAX);
  DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 70, 76, 660, 38,
                  UI_COLOR_PANEL_ALT);
  DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    82, 86, "%s_", lcd_ui_name);

  for (unsigned int row = 0; row < ARRAY_NB(rows); row++) {
    int count = (int)strlen(rows[row]);
    int start_x = (LCD_DISPLAY_WIDTH - count * 62) / 2;
    for (int col = 0; col < count; col++) {
      key[0] = rows[row][col];
      draw_button(buffer, start_x + col * 62, row_y[row], 56, 48, key,
                  UI_COLOR_PANEL_ALT, lcd_ui_name_len < FACE_GALLERY_NAME_MAX);
    }
  }
  draw_button(buffer, 70, 400, 150, 46, "CANCEL", UI_COLOR_PANEL_ALT, 1);
  draw_button(buffer, 250, 400, 150, 46, "BACKSPACE", UI_COLOR_PANEL_ALT,
              lcd_ui_name_len > 0);
  draw_button(buffer, 570, 400, 160, 46, "START", UI_COLOR_PRIMARY,
              lcd_ui_name_len > 0);
}

static void lcd_ui_draw_enrolling(uint8_t *buffer)
{
  face_gallery_enroll_status_t status;
  int progress_width;

  FaceGallery_GetStatus(&status);
  progress_width = status.required_samples > 0
                       ? 420 * status.samples / status.required_samples
                       : 0;
  DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 140, 86, 520, 316,
                  UI_COLOR_PANEL);
  DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    180, 120, "Enrolling: %s", status.name);
  DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    180, 166, "Look at the camera alone");
  DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 190, 218, 420, 30,
                  UI_COLOR_DISABLED);
  if (progress_width > 0)
    DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 190, 218,
                    progress_width, 30, UI_COLOR_PRIMARY);
  DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    330, 264, "Samples: %u/%u", (unsigned int)status.samples,
                    (unsigned int)status.required_samples);
  draw_button(buffer, 190, 330, 180, 48, "CANCEL", UI_COLOR_PANEL_ALT, 1);
  draw_button(buffer, 430, 330, 180, 48, "COMMIT", UI_COLOR_PRIMARY,
              status.samples >= FACE_GALLERY_MIN_SAMPLES);
}

static void lcd_ui_draw_confirmation(uint8_t *buffer, int clear_all)
{
  char name[FACE_GALLERY_NAME_MAX + 1] = {0};
  if (!clear_all)
    (void)FaceGallery_GetName(lcd_ui_delete_slot, name, sizeof(name));
  DRAW_FillArgbHw(buffer, VENC_WIDTH, VENC_HEIGHT, 150, 110, 500, 260,
                  UI_COLOR_PANEL);
  if (clear_all)
    DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                      210, 170, "Delete every identity?");
  else
    DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                      210, 170, "Delete %s?", name);
  DRAW_PrintfArgbHw(&DBG_INFO_FONT, buffer, VENC_WIDTH, VENC_HEIGHT,
                    210, 220, "This writes the persistent gallery.");
  draw_button(buffer, 210, 300, 170, 48, "BACK", UI_COLOR_PANEL_ALT, 1);
  draw_button(buffer, 420, 300, 170, 48, "DELETE", UI_COLOR_DANGER, 1);
}

static void lcd_ui_update_and_draw(uint8_t *buffer)
{
  int touch_x;
  int touch_y;

  if (lcd_touch_get_tap(&touch_x, &touch_y))
    lcd_ui_handle_tap(touch_x, touch_y);

  switch (lcd_ui_page) {
  case LCD_UI_CAMERA:
    draw_button(buffer, 650, 16, 134, 44, "GALLERY", UI_COLOR_PRIMARY, 1);
    break;
  case LCD_UI_GALLERY:
    lcd_ui_draw_gallery(buffer);
    break;
  case LCD_UI_KEYBOARD:
    lcd_ui_draw_keyboard(buffer);
    break;
  case LCD_UI_ENROLLING:
    lcd_ui_draw_enrolling(buffer);
    break;
  case LCD_UI_CONFIRM_DELETE:
    lcd_ui_draw_confirmation(buffer, 0);
    break;
  case LCD_UI_CONFIRM_CLEAR:
    lcd_ui_draw_confirmation(buffer, 1);
    break;
  }
}
#endif

static int clamp_point(int *x, int *y) {
  int xi = *x;
  int yi = *y;

  if (*x < 0)
    *x = 0;
  if (*y < 0)
    *y = 0;
  if (*x >= VENC_WIDTH)
    *x = VENC_WIDTH - 1;
  if (*y >= VENC_HEIGHT)
    *y = VENC_HEIGHT - 1;

  return (xi != *x) || (yi != *y);
}

static void convert_length(float32_t wi, float32_t hi, int *wo, int *ho) {
  *wo = (int)(VENC_WIDTH * wi);
  *ho = (int)(VENC_HEIGHT * hi);
}

static void convert_point(float32_t xi, float32_t yi, int *xo, int *yo) {
  *xo = (int)(VENC_WIDTH * xi);
  *yo = (int)(VENC_HEIGHT * yi);
}

static void cvt_nn_box_to_dp_box(od_pp_outBuffer_t *detect, box_t *box_dp) {
  int xc, yc;
  int x0, y0;
  int x1, y1;
  int w, h;

  convert_point(detect->x_center, detect->y_center, &xc, &yc);
  convert_length(detect->width, detect->height, &w, &h);
  /* CenterFace predicts a tight facial-region box. Pad each side for a more
   * natural HUD box without changing detection, tracking, or FaceID input. */
  w += 2 * MAX(2, w * BOX_PADDING_PERCENT / 100);
  h += 2 * MAX(2, h * BOX_PADDING_PERCENT / 100);
  x0 = xc - (w + 1) / 2;
  y0 = yc - (h + 1) / 2;
  x1 = xc + (w + 1) / 2;
  y1 = yc + (h + 1) / 2;
  clamp_point(&x0, &y0);
  clamp_point(&x1, &y1);

  box_dp->x = x0;
  box_dp->y = y0;
  box_dp->w = x1 - x0;
  box_dp->h = y1 - y0;
  box_dp->conf = detect->conf;
}

static void draw_track_box(uint8_t *p_buffer, TrackObject_s *track) {
  box_t box_disp;

  cvt_nn_box_to_dp_box(&track->dbox, &box_disp);
  DRAW_RectArgbHw(p_buffer, VENC_WIDTH, VENC_HEIGHT, box_disp.x, box_disp.y,
                  box_disp.w, box_disp.h, OBJ_RECT_COLOR);
  if (track->name[0] != '\0') {
    int similarity_percent = (int)(track->recognition_similarity * 100.0f + 0.5f);
    if (similarity_percent < 0) similarity_percent = 0;
    if (similarity_percent > 100) similarity_percent = 100;
    DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, p_buffer, VENC_WIDTH, VENC_HEIGHT,
                      box_disp.x, box_disp.y, "%s %d%%", track->name,
                      similarity_percent);
  } else {
    DRAW_PrintfArgbHw(&CONF_LEVEL_FONT, p_buffer, VENC_WIDTH, VENC_HEIGHT,
                      box_disp.x, box_disp.y, "Unknown ID:%d", track->id);
  }
}

static void draw_rotated_segment(uint8_t *p_buffer, float x0, float y0,
                                 float x1, float y1)
{
  for (int i = 0; i < ROTATED_CORNER_POINTS; i++) {
    float t = (float)i / (float)(ROTATED_CORNER_POINTS - 1);
    int x = (int)(x0 + (x1 - x0) * t + 0.5f) - ROTATED_POINT_SIZE / 2;
    int y = (int)(y0 + (y1 - y0) * t + 0.5f) - ROTATED_POINT_SIZE / 2;
    x = MAX(0, MIN(x, VENC_WIDTH - ROTATED_POINT_SIZE));
    y = MAX(0, MIN(y, VENC_HEIGHT - ROTATED_POINT_SIZE));
    DRAW_FillArgbHw(p_buffer, VENC_WIDTH, VENC_HEIGHT, x, y,
                    ROTATED_POINT_SIZE, ROTATED_POINT_SIZE,
                    ROTATED_RECT_COLOR);
  }
}

static void draw_rotated_corner(uint8_t *p_buffer, float corner_x,
                                float corner_y, float previous_x,
                                float previous_y, float next_x, float next_y)
{
  float previous_arm_x = corner_x +
      (previous_x - corner_x) * ROTATED_CORNER_FRACTION;
  float previous_arm_y = corner_y +
      (previous_y - corner_y) * ROTATED_CORNER_FRACTION;
  float next_arm_x = corner_x +
      (next_x - corner_x) * ROTATED_CORNER_FRACTION;
  float next_arm_y = corner_y +
      (next_y - corner_y) * ROTATED_CORNER_FRACTION;

  draw_rotated_segment(p_buffer, corner_x, corner_y,
                       previous_arm_x, previous_arm_y);
  draw_rotated_segment(p_buffer, corner_x, corner_y,
                       next_arm_x, next_arm_y);
}

static void draw_rotated_face_box(
    uint8_t *p_buffer, od_pp_outBuffer_t *detect,
    const float lm[FACE_NB_LANDMARKS * 2])
{
  box_t box;
  float eye_dx;
  float eye_dy;
  float eye_norm;
  float cosine;
  float sine;
  float cx;
  float cy;
  float half_w;
  float half_h;
  float corner_x[4];
  float corner_y[4];
  static const float local_sign[4][2] = {
    {-1.0f, -1.0f}, {1.0f, -1.0f},
    {1.0f, 1.0f}, {-1.0f, 1.0f}
  };

  /* CenterFace landmark order starts with left and right eye. Their vector
   * supplies roll without requiring atan2/sin/cos at display time. */
  eye_dx = (lm[2] - lm[0]) * VENC_WIDTH;
  eye_dy = (lm[3] - lm[1]) * VENC_HEIGHT;
  eye_norm = sqrtf(eye_dx * eye_dx + eye_dy * eye_dy);
  if (eye_norm < 2.0f)
    return;
  cosine = eye_dx / eye_norm;
  sine = eye_dy / eye_norm;

  cvt_nn_box_to_dp_box(detect, &box);
  cx = (float)box.x + (float)box.w * 0.5f;
  cy = (float)box.y + (float)box.h * 0.5f;
  half_w = (float)box.w * 0.5f;
  half_h = (float)box.h * 0.5f;

  for (int i = 0; i < 4; i++) {
    float local_x = local_sign[i][0] * half_w;
    float local_y = local_sign[i][1] * half_h;
    corner_x[i] = cx + local_x * cosine - local_y * sine;
    corner_y[i] = cy + local_x * sine + local_y * cosine;
  }
  for (int i = 0; i < 4; i++)
    draw_rotated_corner(p_buffer, corner_x[i], corner_y[i],
                        corner_x[(i + 3) % 4], corner_y[(i + 3) % 4],
                        corner_x[(i + 1) % 4], corner_y[(i + 1) % 4]);
}

static void draw_face_landmarks(uint8_t *p_buffer,
                                const float lm[FACE_NB_LANDMARKS * 2])
{
  for (int k = 0; k < FACE_NB_LANDMARKS; k++) {
    int px, py;
    uint32_t color = k < 2 ? EYE_LANDMARK_COLOR :
                     k == 2 ? NOSE_LANDMARK_COLOR :
                              MOUTH_LANDMARK_COLOR;
    convert_point(lm[2 * k], lm[2 * k + 1], &px, &py);
    int x0 = px - LANDMARK_DOT_SZ / 2;
    int y0 = py - LANDMARK_DOT_SZ / 2;
    clamp_point(&x0, &y0);
    DRAW_FillArgbHw(p_buffer, VENC_WIDTH, VENC_HEIGHT,
                    x0, y0, LANDMARK_DOT_SZ, LANDMARK_DOT_SZ,
                    color);
  }
}

static void time_stat_display(time_stat_t *p_stat, uint8_t *p_buffer,
                              char *label, int line_nb, int indent) {
  int offset = VENC_WIDTH - 41 * DBG_INFO_FONT.width;
  int mean_i = (int)p_stat->mean;
  int mean_f = (int)((p_stat->mean - (float)mean_i) * 10.0f);

  DRAW_PrintfArgbHw(&DBG_INFO_FONT, p_buffer, VENC_WIDTH, VENC_HEIGHT, offset,
                    line_nb * DBG_INFO_FONT.height,
                    "%*s%s : %3d ms / %4d.%1d ms ", indent + 1, "", label,
                    p_stat->last, mean_i, mean_f);
}

static int build_display_nn_dbg(uint8_t *p_buffer, stat_info_t *si,
                                int line_nb) {
  time_stat_display(&si->nn_total_time, p_buffer, "NN thread stats  ",
                    line_nb++, 0);
  time_stat_display(&si->nn_inference_time, p_buffer, "inference    ",
                    line_nb++, 4);

  return line_nb;
}

static int build_display_disp_dbg(uint8_t *p_buffer, stat_info_t *si,
                                  int line_nb) {
  time_stat_display(&si->disp_total_time, p_buffer, "DISP thread stats",
                    line_nb++, 0);
  time_stat_display(&si->nn_pp_time, p_buffer, "pp           ", line_nb++, 4);
  time_stat_display(&si->disp_display_time, p_buffer, "display      ",
                    line_nb++, 4);
#if defined(APP_OUTPUT_UVC)
  time_stat_display(&si->disp_enc_time, p_buffer, "encode       ", line_nb++,
                    4);
#else
  time_stat_display(&si->disp_enc_time, p_buffer, "LCD present  ", line_nb++,
                    4);
#endif

  return line_nb;
}

static int update_and_capture_debug_enabled(void) {
  static int prev_button_state = GPIO_PIN_RESET;
  static int display_debug_enabled = 0;
  int cur_button_state;

  cur_button_state = BSP_PB_GetState(BUTTON_USER1);
  if (cur_button_state == GPIO_PIN_SET && prev_button_state == GPIO_PIN_RESET)
    display_debug_enabled = !display_debug_enabled;
  prev_button_state = cur_button_state;

  return display_debug_enabled;
}

static int build_display_inference_info(uint8_t *p_buffer, uint32_t inf_time,
                                        int line_nb) {
  const int offset_x = 16;

  DRAW_PrintfArgbHw(&INF_INFO_FONT, p_buffer, VENC_WIDTH, VENC_HEIGHT, offset_x,
                    line_nb * INF_INFO_FONT.height, " Inference : %4lu ms ",
                    (unsigned long)inf_time);

  return line_nb + 1;
}

static int build_display_cpu_load(uint8_t *p_buffer, int line_nb) {
  const int offset_x = 16;
  float cpu_load_one_second;
  int cpu_int, cpu_frac;

  app_stats_cpuload_get(NULL, &cpu_load_one_second, NULL);
  cpu_int  = (int)cpu_load_one_second;
  cpu_frac = (int)((cpu_load_one_second - (float)cpu_int) * 10.0f);
  DRAW_PrintfArgbHw(&INF_INFO_FONT, p_buffer, VENC_WIDTH, VENC_HEIGHT, offset_x,
                    line_nb * INF_INFO_FONT.height, " Cpu load  : %2d.%1d  %% ",
                    cpu_int, cpu_frac);
  line_nb++;

  return line_nb;
}

static void build_display_stat_info(uint8_t *p_buffer, stat_info_t *si) {
  int line_nb = 1;

  if (!update_and_capture_debug_enabled())
    return;

  line_nb = build_display_nn_dbg(p_buffer, si, line_nb);
  build_display_disp_dbg(p_buffer, si, line_nb);
}

static void build_display(uint8_t *p_buffer, od_pp_out_t *pp_out,
                          float landmarks[][FACE_NB_LANDMARKS * 2],
                          int nb_faces) {
  const uint8_t *fig_array[] = {fig0, fig1, fig2, fig3, fig4,
                                fig5, fig6, fig7, fig8, fig9};
  int line_nb = VENC_HEIGHT / INF_INFO_FONT.height - 4;
  stat_info_t si_copy;
  int nb;

  stat_info_copy(&si_copy);

  {
    TO_DisplayIterator_s it;
    TrackObject_s *track;
    TrackObject_DisplayIterator_Init(&it);
    while ((track = TrackObject_DisplayIterator_GetNext(&it)) != NULL) {
      if (track->is_dbox_valid)
        draw_track_box(p_buffer, track);
    }
  }

  /* Draw landmark points for all detected faces this frame */
  for (int i = 0; i < nb_faces; i++) {
    draw_rotated_face_box(p_buffer, &pp_out->pOutBuff[i], landmarks[i]);
    draw_face_landmarks(p_buffer, landmarks[i]);
  }

  line_nb = build_display_inference_info(
      p_buffer, si_copy.nn_inference_time.last, line_nb);
  line_nb = build_display_cpu_load(p_buffer, line_nb);

  nb = MIN(pp_out->nb_detect, ARRAY_NB(fig_array) - 1);
  DRAW_CopyArgbHW(p_buffer, VENC_WIDTH, VENC_HEIGHT, (uint8_t *)fig_array[nb],
                  64, 64, 16, 16);

  build_display_stat_info(p_buffer, &si_copy);
}

#if defined(APP_OUTPUT_UVC)
static size_t encode_display(int is_intra_force, uint8_t *p_buffer) {
  return ENC_EncodeFrame(p_buffer, venc_out_buffer, VENC_OUT_BUFFER_SIZE,
                         is_intra_force);
}

static int send_display(int len) {
  int ret;

  buffer_flying = 1;
  ret = UVCL_ShowFrame(venc_out_buffer, len);
  if (ret != 0)
    buffer_flying = 0;

  return ret;
}

static void app_uvc_streaming_active(struct uvcl_callbacks *cbs,
                                     UVCL_StreamConf_t stream) {
  (void)cbs;
  (void)stream;
  uvc_is_active = 1;
  BSP_LED_On(LED_RED);
}

static void app_uvc_streaming_inactive(struct uvcl_callbacks *cbs) {
  (void)cbs;
  uvc_is_active = 0;
  BSP_LED_Off(LED_RED);
}

static void app_uvc_frame_release(struct uvcl_callbacks *cbs, void *frame) {
  (void)cbs;
  (void)frame;
  assert(buffer_flying);

  buffer_flying = 0;
}
#endif

void app_display_init(void) {
  int ret;

  dma2d_sem = xSemaphoreCreateCountingStatic(1, 0, &dma2d_sem_buffer);
  assert(dma2d_sem);
  dma2d_lock = xSemaphoreCreateMutexStatic(&dma2d_lock_buffer);
  assert(dma2d_lock);

  ret = DRAW_FontSetup(&Font12, &font_12);
  assert(ret == 0);
  ret = DRAW_FontSetup(&Font16, &font_16);
  assert(ret == 0);

#if defined(APP_OUTPUT_UVC)
  buffer_flying = 0;
  force_intra = 0;
  uvc_is_active = 0;
#else
  lcd_is_active = 0;
  lcd_write_index = 0;
  lcd_touch_available = 0;
  lcd_touch_latched = 0;
  lcd_touch_address = 0;
  lcd_ui_page = LCD_UI_CAMERA;
  lcd_ui_delete_slot = 0;
  lcd_ui_name_len = 0;
  memset(lcd_ui_name, 0, sizeof(lcd_ui_name));
  memset(lcd_ui_message, 0, sizeof(lcd_ui_message));
#endif
}

int app_display_setup(void) {
  int ret;

#if defined(APP_OUTPUT_UVC)
  UVCL_Conf_t uvcl_conf = { 0 };
  ENC_Conf_t enc_conf = { 0 };

  enc_conf.width = VENC_WIDTH;
  enc_conf.height = VENC_HEIGHT;
  enc_conf.fps = UVC_STREAM_FPS;
  enc_conf.gop_frames = VENC_GOP_FRAMES;
  ENC_Init(&enc_conf);

  uvcl_conf.streams[0].width = VENC_WIDTH;
  uvcl_conf.streams[0].height = VENC_HEIGHT;
  uvcl_conf.streams[0].fps = UVC_STREAM_FPS;
  uvcl_conf.streams[0].payload_type = UVCL_PAYLOAD_FB_H264;
  uvcl_conf.streams_nb = 1;
  /* Pace transfers to the advertised rate so host timestamps stay monotonic
   * and media frameworks do not accumulate a corrective jitter buffer. */
  uvcl_conf.is_immediate_mode = 0;

  uvcl_cbs.streaming_active = app_uvc_streaming_active;
  uvcl_cbs.streaming_inactive = app_uvc_streaming_inactive;
  uvcl_cbs.frame_release = app_uvc_frame_release;
  ret = UVCL_Init(USB1_OTG_HS, &uvcl_conf, &uvcl_cbs);
#else
  lcd_touch_init();
  ret = BSP_LCD_InitEx(0, LCD_ORIENTATION_LANDSCAPE,
                       LCD_PIXEL_FORMAT_ARGB8888,
                       LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT);
#endif

  return ret;
}

int app_display_render(uint8_t *frame_buffer, od_pp_out_t *pp_out,
                       float landmarks[][FACE_NB_LANDMARKS * 2], int nb_faces) {
#if defined(APP_OUTPUT_UVC)
  static int uvc_is_active_prev = 0;
#endif
  stat_info_t *stats = app_stats_state();
  uint32_t ts;
#if defined(APP_OUTPUT_UVC)
  int len;

  if (!uvc_is_active) {
    uvc_is_active_prev = uvc_is_active;
    return 0;
  }

  /* Do not touch a rotating camera buffer unless this invocation can encode
   * it. Drawing frames that USB will drop creates avoidable DMA2D/DCMIPP
   * ownership races and makes the HUD appear on alternating frames. */
  if (buffer_flying)
    return 0;
#else
  int ret;
#endif

  ts = HAL_GetTick();
#if defined(APP_OUTPUT_UVC)
  build_display(frame_buffer, pp_out, landmarks, nb_faces);
#else
  /* Snapshot the camera image first, then compose only into the LCD-owned
   * buffer. DCMIPP may reuse its capture buffer at any time after the frame
   * event, so drawing there before copying lets it erase HUD pixels mid-copy. */
  DRAW_CopyArgbHW(lcd_frame_buffer[lcd_write_index],
                  LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT,
                  frame_buffer, VENC_WIDTH, VENC_HEIGHT, 0, 0);
  build_display(lcd_frame_buffer[lcd_write_index], pp_out, landmarks,
                nb_faces);
  lcd_ui_update_and_draw(lcd_frame_buffer[lcd_write_index]);
#endif

  stats_detect_update(pp_out->nb_detect);

  time_stat_update(&stats->disp_display_time, HAL_GetTick() - ts);

#if defined(APP_OUTPUT_UVC)
  ts = HAL_GetTick();
  len = (int)encode_display(!uvc_is_active_prev || force_intra, frame_buffer);
  time_stat_update(&stats->disp_enc_time, HAL_GetTick() - ts);

  if (len > 0)
    send_display(len);

  force_intra = 0;
  uvc_is_active_prev = uvc_is_active;

  return uvc_is_active;
#else
  /* Queue the new framebuffer address for the next vertical blank so LTDC
   * never switches buffers halfway through a scanout. */
  ts = HAL_GetTick();
  ret = BSP_LCD_Reload(0, BSP_LCD_RELOAD_NONE);
  assert(ret == BSP_ERROR_NONE);
  ret = BSP_LCD_SetLayerAddress(0, 0,
                                (uint32_t)lcd_frame_buffer[lcd_write_index]);
  assert(ret == BSP_ERROR_NONE);
  ret = BSP_LCD_Reload(0, lcd_is_active ? BSP_LCD_RELOAD_VERTICAL_BLANKING
                                        : BSP_LCD_RELOAD_IMMEDIATE);
  assert(ret == BSP_ERROR_NONE);
  if (!lcd_is_active) {
    ret = BSP_LCD_DisplayOn(0);
    assert(ret == BSP_ERROR_NONE);
    lcd_is_active = 1;
  }
  lcd_write_index = (lcd_write_index + 1U) % LCD_FRAME_BUFFER_COUNT;
  time_stat_update(&stats->disp_enc_time, HAL_GetTick() - ts);

  return 1;
#endif
}

void DRAW_HwLock(void *dma2d_handle) {
  int ret;

  (void)dma2d_handle;
  ret = xSemaphoreTake(dma2d_lock, portMAX_DELAY);
  assert(ret == pdTRUE);
}

void DRAW_HwUnlock(void) {
  int ret;

  ret = xSemaphoreGive(dma2d_lock);
  assert(ret == pdTRUE);
}

void DRAW_Wfe(void) {
  int ret;

  ret = xSemaphoreTake(dma2d_sem, portMAX_DELAY);
  assert(ret == pdTRUE);
}

void DRAW_Signal(void) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  int ret;

  ret = xSemaphoreGiveFromISR(dma2d_sem, &xHigherPriorityTaskWoken);
  assert(ret == pdTRUE);

  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
