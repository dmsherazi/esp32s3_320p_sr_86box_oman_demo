/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "driver/i2s.h"
#include "esp_task_wdt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "dl_lib_coefgetter_if.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "driver/i2s.h"
#include "model_path.h"
#include "app_sr_handler.h"
#include "app_sr.h"
#include "board_pin.h"

#define TAG "app_sr"

/*
 * esp-asr入门指南(Getting Started)
 * https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/getting_started/readme.html
*/



int detect_flag = 0;

typedef struct {
    sr_language_t lang;
    model_iface_data_t *model_data;
    const esp_mn_iface_t *multinet;
    const esp_afe_sr_iface_t *afe_handle;
    esp_afe_sr_data_t *afe_data;
    int16_t *afe_in_buffer;
    int16_t *afe_out_buffer;
    SLIST_HEAD(sr_cmd_list_t, sr_cmd_t) cmd_list;
    uint8_t cmd_num;
    TaskHandle_t feed_task;
    TaskHandle_t detect_task;
    TaskHandle_t handle_task;
    QueueHandle_t result_que;
    EventGroupHandle_t event_group;

    FILE *fp;
    bool b_record_en;
} sr_data_t;

static sr_data_t *g_sr_data = NULL;
#define NEED_DELETE 	BIT0
#define FEED_DELETED 	BIT1
#define DETECT_DELETED 	BIT2

extern int board_get_feed_channel(void);
//extern esp_err_t board_get_feed_data(int16_t *buffer, int buffer_len);
extern esp_err_t board_get_feed_data(int16_t *buffer, int chunksize);

static bool g_feed_flag = true;
static bool g_detect_flag = true;


//任务://从硬件i2s读取数据给afe
void feed_Task(void *arg)
{
	const esp_afe_sr_iface_t *afe_handle = g_sr_data->afe_handle;
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *) arg;
    //支持音频格式为 16 KHz，16 bit，单通道。AFE fetch 拿到的数据也为这个格式
    //get_feed_chunksize()确定需要传入 MultiNet 的帧长
    //返回值audio_chunksize是需要传入MultiNet的每帧音频的short型点数，这个大小和AFE中fetch的每帧数据点数完全一致
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = board_get_feed_channel();
    /* Allocate audio buffer and check for result */
    uint32_t read_size = audio_chunksize * sizeof(int16_t) * (feed_channel + 1);
    ESP_LOGI(TAG, "audio_chunksize=%d, feed_channel=%d,read_size=%d", audio_chunksize, feed_channel,read_size);

    int16_t* feed_buf = heap_caps_calloc(1, read_size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);//The DMA used for data transfer can only use internal RAM
    assert(feed_buf);

    while (g_feed_flag)
    {
        esp_err_t ret;
        ret = board_get_feed_data(feed_buf, audio_chunksize);//2S reads data through DMA mode

        /* Feed samples of an audio stream to the AFE_SR */
        if (ret == ESP_OK)
            afe_handle->feed(afe_data, feed_buf);//数据给afe处理
        else
            vTaskDelay(pdMS_TO_TICKS(150));
    }

    afe_handle->destroy(afe_data);
    free(feed_buf);
    g_detect_flag = false;
    vTaskDelete(NULL);
}


//任务:侦测唤醒词和命令
void detect_Task(void *arg)
{
	const esp_afe_sr_iface_t *afe_handle = g_sr_data->afe_handle;

    esp_afe_sr_data_t *afe_data = arg;
    /* Allocate buffer for detection */
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    int16_t *buff =  heap_caps_calloc(1, afe_chunksize * sizeof(int16_t), MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);//The DMA used for data transfer can only use internal RAM
    assert(buff);

    //load model:esp-sr-->kconfig-->speech commands
	static const esp_mn_iface_t *multinet = &MULTINET_MODEL;
	model_iface_data_t *model_data = multinet->create((model_coeff_getter_t *)&MULTINET_COEFF, 5760);
	int mu_chunksize = multinet->get_samp_chunksize(model_data);
	assert(mu_chunksize == afe_chunksize);

    while (g_detect_flag)
    {
        if (!g_feed_flag)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
       // the output state of fetch function
        int res = afe_handle->fetch(afe_data, buff);

        if (res == AFE_FETCH_WWE_DETECTED)
        {
            printf("wakeword detected\n");
            printf("-----------LISTENING-----------\n");

            sr_result_t result = {
                .fetch_mode = res,
                .state = ESP_MN_STATE_DETECTING,
                .command_id = -1,
            };
            xQueueSend(g_sr_data->result_que, &result, 0);
        }

        if (res == AFE_FETCH_CHANNEL_VERIFIED)
        {
        	printf("Channel verified\n");

            detect_flag = 1;
            afe_handle->disable_wakenet(afe_data);//Stop the operation of WakeNet after waking up, thereby reducing CPU loading
        } 

        if (detect_flag == 1)
        {
        	if (false == sr_echo_is_playing())//Is the voice playing?//Due to lack of support for AEC function, voice recognition is not performed during playback
        	{
				int command_id = multinet->detect(model_data, buff);
				if (command_id >= -2)
				{
					if (command_id > -1)
					{
						printf("Deteted command_id: %d\n", command_id);

		                sr_result_t result = {
		                    .fetch_mode = res,
		                    .state = ESP_MN_STATE_DETECTED,
		                    .command_id = command_id,
		                };
		                xQueueSend(g_sr_data->result_que, &result, 0);

						afe_handle->enable_wakenet(afe_data);//Speech recognition successful, entering wake-up mode again
						detect_flag = 0;
						printf("\n-----------awaits to be waken up-----------\n");
					}

					if (command_id == -2)
					{
						printf("Time out\n");
			                sr_result_t result = {
			                    .fetch_mode = res,
			                    .state = ESP_MN_STATE_TIMEOUT,
			                    .command_id = -1,
			                };
			                xQueueSend(g_sr_data->result_que, &result, 0);

						afe_handle->enable_wakenet(afe_data);//Speech recognition timeout, entering wake-up mode again
						detect_flag = 0;
						printf("\n-----------awaits to be waken up-----------\n");
					}
				}
			}
        }
    }
    afe_handle->destroy(afe_data);
    free(buff);
    vTaskDelete(NULL);
}

esp_err_t app_sr_get_result(sr_result_t *result, TickType_t xTicksToWait)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");

    xQueueReceive(g_sr_data->result_que, result, xTicksToWait);
    return ESP_OK;
}

const sr_cmd_t *app_sr_get_cmd_from_id(uint32_t id)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, NULL, TAG, "SR is not running");
    ESP_RETURN_ON_FALSE(id < g_sr_data->cmd_num, NULL, TAG, "cmd id out of range");

    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        if (id == it->id) {
            return it;
        }
    }
    ESP_RETURN_ON_FALSE(NULL != it, NULL, TAG, "can't find cmd id:%d", id);
    return NULL;
}


esp_err_t app_sr_model_init(void)
{
	esp_err_t ret = ESP_OK;
    srmodel_spiffs_init();
    return ESP_OK;
}

esp_err_t app_sr_init(void)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(NULL == g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR already running");

    g_sr_data = heap_caps_calloc(1, sizeof(sr_data_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_NO_MEM, TAG, "Failed create sr data");

    g_sr_data->result_que = xQueueCreate(3, sizeof(sr_result_t));
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->result_que, ESP_ERR_NO_MEM, err, TAG, "Failed create result queue");

    g_sr_data->event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->event_group, ESP_ERR_NO_MEM, err, TAG, "Failed create event_group");

    esp_task_wdt_reset();

    //afe_handle = &esp_afe_sr_2mic;
    g_sr_data->afe_handle =&esp_afe_sr_2mic;

    afe_config_t afe_config = {
        .aec_init = false,
        .se_init = true,
        .vad_init = true,
        .wakenet_init = true,
        .vad_mode = 3,
        .wakenet_model = &WAKENET_MODEL,
        .wakenet_coeff = (model_coeff_getter_t *)&WAKENET_COEFF,
        .wakenet_mode = DET_MODE_2CH_90,
        .afe_mode = SR_MODE_LOW_COST,
        .afe_perferred_core = 0,
        .afe_perferred_priority = 5,
        .afe_ringbuf_size = 40,
        .alloc_from_psram = AFE_PSRAM_LOW_COST,
        .agc_mode = 3,
    };

    g_sr_data->afe_data= g_sr_data->afe_handle->create_from_config(&afe_config);
    xTaskCreatePinnedToCore(&feed_Task, "feed", 4 * 1024, (void*)g_sr_data->afe_data, 7, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 6 * 1024, (void*)g_sr_data->afe_data, 7, NULL, 1);
    xTaskCreatePinnedToCore(&sr_handler_task, "sr_handler", 4 * 1024, (void*)g_sr_data->afe_data, 7, NULL, 1);

    return ESP_OK;
err:
	//app_sr_stop();
	return ret;
}
esp_err_t app_sr_stop(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    /**
     * Waiting for all task stoped
     * TODO: A task creation failure cannot be handled correctly now
     * */
    xEventGroupSetBits(g_sr_data->event_group, NEED_DELETE);
    xEventGroupWaitBits(g_sr_data->event_group, NEED_DELETE | FEED_DELETED | DETECT_DELETED, 1, 1, portMAX_DELAY);

    if (g_sr_data->result_que) {
        vQueueDelete(g_sr_data->result_que);
        g_sr_data->result_que = NULL;
    }

    if (g_sr_data->event_group) {
        vEventGroupDelete(g_sr_data->event_group);
        g_sr_data->event_group = NULL;
    }

    if (g_sr_data->fp) {
        fclose(g_sr_data->fp);
        g_sr_data->fp = NULL;
    }

    if (g_sr_data->model_data) {
        g_sr_data->multinet->destroy(g_sr_data->model_data);
    }

    if (g_sr_data->afe_data) {
        g_sr_data->afe_handle->destroy(g_sr_data->afe_data);
    }

    sr_cmd_t *it;
    while (!SLIST_EMPTY(&g_sr_data->cmd_list)) {
        it = SLIST_FIRST(&g_sr_data->cmd_list);
        SLIST_REMOVE_HEAD(&g_sr_data->cmd_list, next);
        heap_caps_free(it);
    }

    if (g_sr_data->afe_in_buffer) {
        heap_caps_free(g_sr_data->afe_in_buffer);
    }

    if (g_sr_data->afe_out_buffer) {
        heap_caps_free(g_sr_data->afe_out_buffer);
    }

    heap_caps_free(g_sr_data);
    g_sr_data = NULL;
    return ESP_OK;
}
void wwe_stop(void)
{
    g_feed_flag = false;
    vTaskDelay(pdMS_TO_TICKS(5));
}


