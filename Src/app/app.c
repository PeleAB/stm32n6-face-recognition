/**
 ******************************************************************************
 * @file    app.c
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

#include "app/app.h"

#include <assert.h>
#include <stdio.h>

#include "app/app_config.h"
#include "app/app_pipeline.h"
#include "svc/app_display.h"
#include "svc/app_stats.h"
#include "stm32n6570_discovery.h"

void app_run(void)
{
  int ret;

  printf("Init application\n");
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  ret = BSP_PB_Init(BUTTON_USER1, BUTTON_MODE_GPIO);
  assert(ret == BSP_ERROR_NONE);

  app_stats_init();
  app_display_init();
  app_pipeline_init();

  CAM_Init();

  ret = app_display_setup();
  assert(ret == 0);

  app_pipeline_start();

  BSP_LED_On(LED_GREEN);
}

