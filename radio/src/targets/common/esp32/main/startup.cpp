/*
* Copyright (C) OpenTX
*
* Based on code named
*   th9x - http://code.google.com/p/th9x
*   er9x - http://code.google.com/p/er9x
*   gruvin9x - http://code.google.com/p/gruvin9x
*
* License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include <stdio.h>
#include "esp_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task.h"
#include "soc/timer_group_struct.h"
#include "driver/periph_ctrl.h"
#include "driver/timer.h"
#include "esp_system.h"
#include "esp_timer.h"
//#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
#include "esp_spiffs.h"
#define HASASSERT
#include "opentx.h"

#define AUDIO_TASK_CORE 0
#define PER10MS_TASK_CORE 0
#define ENC_TASK_CORE 0

TaskPrio mixerTaskPrio = {ESP_TASK_PRIO_MAX -6, MIXER_TASK_CORE};
TaskPrio menuTaskPrio = {ESP_TASK_PRIO_MAX -9, MENU_TASK_CORE};
TaskPrio audioTaskPrio = {ESP_TASK_PRIO_MAX -8, MENU_TASK_CORE};
static const char *TAG = "startup.cpp";
extern TaskHandle_t menusTaskId;
extern TaskHandle_t mixerTaskId;
extern TaskHandle_t audioTaskId;

TaskHandle_t xPer10msTaskHandle = NULL;
TaskHandle_t xEncTaskHandle = NULL;
extern TaskHandle_t wifiTaskHandle;
extern TaskHandle_t otaTaskHandle;
extern TaskHandle_t xInitPulsesTaskId;

SemaphoreHandle_t xPer10msSem = NULL;
extern SemaphoreHandle_t mixerMutex;
extern SemaphoreHandle_t audioMutex;
//uint16_t testDuration;

uint16_t encStackAvailable()
{
    return uxTaskGetStackHighWaterMark(xEncTaskHandle);
}

uint16_t menusStackAvailable()
{
    return uxTaskGetStackHighWaterMark(menusTaskId);
}

uint16_t mixerStackAvailable()
{
    return uxTaskGetStackHighWaterMark(mixerTaskId);
}

uint16_t per10msStackAvailable()
{
    return uxTaskGetStackHighWaterMark(xPer10msTaskHandle);
}

uint16_t audioStackAvailable()
{
    return uxTaskGetStackHighWaterMark(audioTaskId);
}


uint16_t getTmr2MHz()
{
    return ((uint16_t) esp_timer_get_time())*2;
}

void pauseMixerCalculations() {
  xSemaphoreTake(mixerMutex, portMAX_DELAY);
}

void resumeMixerCalculations() {
  xSemaphoreGive(mixerMutex);
}

void  per10msTask(void * pdata)
{
    while(1) {
        xSemaphoreTake(xPer10msSem, portMAX_DELAY);
        //        uint32_t now = esp_timer_get_time();
        per10ms();
        READI2CSW();
        //        testDuration = (uint16_t)(esp_timer_get_time()-now);
    }
}


void espTasksStart()
{
    BaseType_t ret;

    ESP_LOGI(TAG,"Starting tasks.");

    ret=xTaskCreatePinnedToCore( per10msTask, "per10msTask", PER10MS_STACK_SIZE, NULL, ESP_TASK_PRIO_MAX -5, &xPer10msTaskHandle, PER10MS_TASK_CORE );
    configASSERT( xPer10msTaskHandle );
#if defined(MCP23017_ADDR_SW)
    ret=xTaskCreatePinnedToCore( encoderTask, "encoderTask", ENC_STACK_SIZE, NULL, ESP_TASK_PRIO_MAX -4, &xEncTaskHandle, ENC_TASK_CORE );
    configASSERT( xEncTaskHandle );
#endif
}

void rtosInit() {
  audioMutex = xSemaphoreCreateMutex();
  if( audioMutex == NULL ) {
    ESP_LOGE(TAG,"Failed to create semaphore: audioMutex");
  }
  mixerMutex = xSemaphoreCreateMutex();
  if( mixerMutex == NULL ) {
    ESP_LOGE(TAG,"Failed to create semaphore: mixerMutex");
  }
  espTasksStart();
}


void IRAM_ATTR timer_group0_isr(void *para)
{
    int timer_idx = (int) para;

    /* Retrieve the interrupt status and the counter value
    from the timer that reported the interrupt */
    uint32_t intr_status = TIMERG0.int_st_timers.val;
    TIMERG0.hw_timer[timer_idx].update = 1;

    /* Clear the interrupt
    and update the alarm time for the timer with reload */
    if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_0) {
        TIMERG0.int_clr_timers.t0 = 1;
    } else if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_1) {
        TIMERG0.int_clr_timers.t1 = 1;
    }

    /* After the alarm has been triggered
    we need enable it again, so it is triggered the next time */
    TIMERG0.hw_timer[timer_idx].config.alarm_en = TIMER_ALARM_EN;

    //    if(NULL!=xPer10msSem){
    BaseType_t mustYield=false;
    xSemaphoreGiveFromISR(xPer10msSem, &mustYield);
    if (mustYield) portYIELD_FROM_ISR();
    //    }
}

static void tg0_timer_init(timer_idx_t timer_idx)
{
    /* Select and initialize basic parameters of the timer */
    timer_config_t config;
    memset(&config, 0, sizeof(config));
    config.divider = 16; // TIMER_BASE_CLK/16
    config.counter_dir = TIMER_COUNT_UP;
    config.counter_en = TIMER_PAUSE;
    config.alarm_en = TIMER_ALARM_EN;
    config.intr_type = TIMER_INTR_LEVEL;
    config.auto_reload = TIMER_AUTORELOAD_EN;
    timer_init(TIMER_GROUP_0, timer_idx, &config);

    /* Timer's counter will initially start from value below.
    Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(TIMER_GROUP_0, timer_idx, 0x00000000ULL);

    xPer10msSem = xSemaphoreCreateBinary();
    if( xPer10msSem == NULL ) {
        ESP_LOGE(TAG,"Failed to create semaphore: xPer10msSem.");
        return;
    }

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(TIMER_GROUP_0, timer_idx, TIMER_BASE_CLK/(16*100)); //100Hz
    timer_enable_intr(TIMER_GROUP_0, timer_idx);
    timer_isr_register(TIMER_GROUP_0, timer_idx, timer_group0_isr,
    (void *) timer_idx, ESP_INTR_FLAG_IRAM, NULL);
    timer_start(TIMER_GROUP_0, timer_idx);
}

void timer10msInit()
{
    ESP_LOGI(TAG,"Starting 10ms timer.");
    tg0_timer_init(TIMER_0); //10 ms interrupt
}

void espLogI(const char * format, ...)
{
    va_list arglist;
    va_start(arglist, format);
    vprintf( format, arglist);
    va_end(arglist);
}

#define LOGBUFFLEN 255
char logBuff[LOGBUFFLEN]="\0";
void espLogPut(const char * format, ...)
{
    va_list arglist;
    va_start(arglist, format);
    vsnprintf( logBuff,LOGBUFFLEN,format, arglist);
    va_end(arglist);
}


#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)

void vTaskGetRunTimeStatsA( )
{
    TaskStatus_t *pxTaskStatusArray;
    volatile UBaseType_t uxArraySize, x;
    uint32_t ulTotalRunTime, ulStatsAsPercentage;

    // Take a snapshot of the number of tasks in case it changes while this
    // function is executing.
    uxArraySize = uxTaskGetNumberOfTasks();

    // Allocate a TaskStatus_t structure for each task.  An array could be
    // allocated statically at compile time.
    pxTaskStatusArray = (TaskStatus_t *) pvPortMalloc( uxArraySize * sizeof( TaskStatus_t ) );

    if( pxTaskStatusArray != NULL )
    {
        // Generate raw status information about each task.
        uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, uxArraySize, &ulTotalRunTime );

        // For percentage calculations.
        ulTotalRunTime /= 100UL;

        // Avoid divide by zero errors.
        if( ulTotalRunTime > 0 )
        {
            // For each populated position in the pxTaskStatusArray array,
            // format the raw data as human readable ASCII data
            for( x = 0; x < uxArraySize; x++ )
            {
                // What percentage of the total run time has the task used?
                // This will always be rounded down to the nearest integer.
                // ulTotalRunTimeDiv100 has already been divided by 100.
                ulStatsAsPercentage = pxTaskStatusArray[ x ].ulRunTimeCounter / ulTotalRunTime;

                if( ulStatsAsPercentage > 0UL )
                {
                    ESP_LOGI(TAG, "stat: %-16s/t%12u/t%u%%", pxTaskStatusArray[ x ].pcTaskName, pxTaskStatusArray[ x ].ulRunTimeCounter, ulStatsAsPercentage );
                }
                else
                {
                    // If the percentage is zero here then the task has
                    // consumed less than 1% of the total run time.
                    ESP_LOGI(TAG, "stat: %-16s/t%12u/t<1%%", pxTaskStatusArray[ x ].pcTaskName, pxTaskStatusArray[ x ].ulRunTimeCounter );
                }
            }
        }

        // The array is no longer needed, free the memory it consumes.
        vPortFree( pxTaskStatusArray );
    }
}
#endif
int main();
extern volatile uint32_t testCount;
extern volatile uint32_t testTime;
extern volatile uint32_t ppmMixDly;

extern "C"   void otx_main()
{
    main();
    eeBackupAll();
    while(1) {
//        isWiFiStarted();
        ESP_LOGD(TAG,"s_pulses_paused: %d",s_pulses_paused);
//        ESP_LOGI(TAG,"");
//        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
#if false
        ESP_LOGI(TAG,"");
        TaskHandle_t *tasks[]= {&menusTaskId,&mixerTaskId,&audioTaskId,&xPer10msTaskHandle,&xEncTaskHandle,&wifiTaskHandle, &otaTaskHandle, & xInitPulsesTaskId};
        uint8_t nTasks= sizeof(tasks)/sizeof(tasks[0]);
        ESP_LOGI(TAG,"");
        ESP_LOGI(TAG,"time elapsed %f ms:", esp_timer_get_time()/1000.);
        for(uint8_t i=0; i< nTasks; i++) {
            if( NULL != *tasks[i] ){
                ESP_LOGI(TAG,"Min stack: %s: %d",pcTaskGetTaskName(*tasks[i]),uxTaskGetStackHighWaterMark(*tasks[i]));
            }
        }
#endif
        ESP_LOGD(TAG,"maxMixerDuration: %d us.",maxMixerDuration);
        if(*logBuff){
            ESP_LOGI(TAG,"logPut:'%s'",logBuff);
            *logBuff=0;
        }
//        ESP_LOGI(TAG,"PPM frames: count: %d, delay: %d, ppmMixDly: %d",testCount,testTime,ppmMixDly);
//        testCount=0;
//        ESP_LOGD(TAG,"last 10ms task duration %d",testDuration);
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)        
        ESP_LOGI(TAG,"");
        vTaskGetRunTimeStatsA();
#endif
        vTaskDelay(2000/portTICK_PERIOD_MS);
    };
}
