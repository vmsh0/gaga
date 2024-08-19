#include <sys/cdefs.h>
#include <assert.h>
#include <sys/time.h>
#include <freertos/ringbuf.h>
#include <FreeRTOSConfig.h>
#include <freertos/portmacro.h>
#include <freertos/projdefs.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <hal/i2s_types.h>
#include <driver/i2s_common.h>
#include <driver/i2s_std.h>
#include <esp_log.h>

//#define STREAM_EMBEDDED_DATA
//#define SOURCE_TASK_EMBEDDED_DATA

#define MINIMP3_ONLY_MP3
#define MINIMP3_NONSTANDARD_BUT_LOGICAL
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "data.h"
#include "streaming.h"
#include "checksum.h"

static const char *TAG = "a_main";

#define SOURCE_STACK_SIZE 4096
#define DECODER_STACK_SIZE 8192
#define SINK_STACK_SIZE 4096
#define MP3_RINGBUF_SIZE (1024*8)
#define MP3_DECODER_BUF_SIZE (1024*24)
#define AUDIO_BUF_SIZE MINIMP3_MAX_SAMPLES_PER_FRAME
#define MAX_SEEK_RETIES 10

struct signals {
  TaskHandle_t sink;
  TaskHandle_t decoder;
} signals;

_Noreturn void sink_task(void *param);
_Noreturn void decoder_task(void *param);

volatile uint16_t buf[AUDIO_BUF_SIZE];  /* note: mp3dec will write *signed* data in here */
volatile size_t useful_size = 0;  /* used by the source to signal available bytes */

#ifdef SOURCE_TASK_EMBEDDED_DATA
_Noreturn void decoder_task(void *param) {
	mp3dec_frame_info_t info;
	size_t cur_pos = 0;
	size_t samples, retries;

	/* init MP3 decoder */
	static mp3dec_t mp3d;
	mp3dec_init(&mp3d);

	/* get MP3 data pointer */
	const uint8_t *audio_data = audio_data_start;
	size_t audio_data_len = audio_data_end - audio_data_start;

	/* ESP_LOG_BUFFER_HEXDUMP(TAG, audio_data, 512, ESP_LOG_DEBUG); */
	ESP_LOGD(TAG, "Buffer size: %d (%p - %p)", audio_data_len, audio_data_end, audio_data_start);

	while (1) {
		xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

		samples = 0;
		retries = MAX_SEEK_RETIES;

		while (samples == 0 && cur_pos != audio_data_len && --retries) {
			info.frame_bytes = 0;
			samples = mp3dec_decode_frame(&mp3d,
										  audio_data + cur_pos, audio_data_len - cur_pos,
										  (mp3d_sample_t *)buf, &info);
			cur_pos += info.frame_bytes;
		}

		if (!retries) {
			ESP_LOGE(TAG, "Fail! resetting source");
			cur_pos = 0;  /* reset */
		} else {
			ESP_LOGD(TAG, "Decode info: ch=%d br=%d hz=%d", info.channels, info.bitrate_kbps, info.hz);
			if (cur_pos >= audio_data_len) {
				cur_pos = 0;
				ESP_LOGD(TAG, "Buf rst");
			}
		}

		useful_size = sizeof(mp3d_sample_t) * samples;
		xTaskNotify(signals.sink, 0, eNoAction);
	}
}
#else

uint8_t mp3_decoder_buf[MP3_DECODER_BUF_SIZE];
int mp3_decoder_buf_curr_size = 0;

#define SYNCHRONIZATION_BYTES (MP3_DECODER_BUF_SIZE * 2 / 3)

void decoder__queue_to_decoder_buffer(RingbufHandle_t rb, int synchronized) {
	size_t dequeue_size;
	uint8_t *dequeue_buf;

	/* determine how many bytes to read at most.
	 * the idea is that if we're synchronized, we're happy with whatever comes (as we use a delay of 0).
	 * if we are not, however, the task logic will keep calling us, so we have to block and yield or other tasks will
	 * not run. as such, we have to request a value that actually makes sense to avoid waiting indefinitely, and that is
	 * SYNCHRONIZATION_BYTES. */
	size_t max_bytes_to_read = MP3_DECODER_BUF_SIZE - mp3_decoder_buf_curr_size;;
	if (!synchronized && max_bytes_to_read > SYNCHRONIZATION_BYTES)
		max_bytes_to_read = SYNCHRONIZATION_BYTES;

	/* get buffer from the queue */
	dequeue_buf = xRingbufferReceiveUpTo(rb,
										 &dequeue_size,
										 synchronized ? (TickType_t)0 : portMAX_DELAY,
										 max_bytes_to_read);

	/* this isn't really documented, but if the queue is empty dequeue_size will be populated with a bogus value; as
	 * such, we must check the pointer itself to see if an item was returned */
	if (dequeue_buf != NULL) {
		assert(dequeue_size + mp3_decoder_buf_curr_size <= MP3_DECODER_BUF_SIZE);

		/* copy the data from the queue to our buffer */
		memcpy(mp3_decoder_buf + mp3_decoder_buf_curr_size, dequeue_buf, dequeue_size);
		/* cast uint8_t to int -- can't do much about this, FreeRTOS and minimp3 use different data types */
		mp3_decoder_buf_curr_size += dequeue_size;

		/* signal to the queue that we're done */
		vRingbufferReturnItem(rb, dequeue_buf);
	}
}

_Noreturn void decoder_task(void *param) {
	RingbufHandle_t rb = (RingbufHandle_t)param;

	mp3dec_frame_info_t info;
	size_t samples, retries;
	int synchronized = 0;  /* is the decoder currently synchronized? */
	uint8_t mp3_frame_ck;  /* debug */
	uint64_t mp3_abs_position = 0;  /* debug */

	/* init MP3 decoder */
	static mp3dec_t mp3d;  /* don't allocate this on the stack, as it's absolutely huge */
	mp3dec_init(&mp3d);

	ESP_LOGD(TAG, "Starting SOURCE task");

	while (1) {
		/* wait for the streaming task to give us a good amount of data if we're not synchronized yet */
		do {
			decoder__queue_to_decoder_buffer(rb, synchronized);
		} while (!synchronized && mp3_decoder_buf_curr_size < SYNCHRONIZATION_BYTES);

		/* wait for sink task to give us access to the pcm buffer */
		xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

		samples = 0;
		retries = MAX_SEEK_RETIES;

		while (samples == 0 && --retries) {
			/* decode some bytes */
			info.frame_bytes = 0;
			samples = mp3dec_decode_frame(&mp3d,
			                              mp3_decoder_buf, mp3_decoder_buf_curr_size,
			                              (mp3d_sample_t *)buf, &info);
			mp3_frame_ck = checksum(mp3_decoder_buf, info.frame_bytes);

			/* use up the bytes in the source buffer */
			memmove(mp3_decoder_buf, mp3_decoder_buf + info.frame_bytes, mp3_decoder_buf_curr_size - info.frame_bytes);
			mp3_decoder_buf_curr_size -= info.frame_bytes;
			mp3_abs_position += info.frame_bytes;
		}

		if (!retries) {
			ESP_LOGE(TAG, "Sync fail!");
			mp3_decoder_buf_curr_size = 0;  /* flush source data */
			useful_size = 0;  /* don't send anything to sink */
			synchronized = 0;  /* we're not synchronized to the mp3 stream anymore */
		} else {
			synchronized = 1;
			useful_size = sizeof(mp3d_sample_t) * samples;
			ESP_LOGV(TAG, "Decode: %d mp3 -> %d PCM -- ch=%d br=%d hz=%d -- %llx: in ck %x, next ck %x, out ck %x",
					 info.frame_bytes, samples,
					 info.channels, info.bitrate_kbps, info.hz,
					 mp3_abs_position,
					 mp3_frame_ck,
					 checksum(mp3_decoder_buf, 500),
					 checksum((uint8_t*)buf, useful_size));
		}

		/* get sink task to consume new pcm data */
		xTaskNotify(signals.sink, 0, eNoAction);
	}
}
#endif

/* this does three things:
 * 1) it turns stereo into mono
 * 2) it swaps around every two mono samples, as required by the DMA
 * 3) it inverts the MSB of each sample, converting to unsigned PCM <- removed, because it causes clipping. what? */
void audio_sbramangle_mono_data() {
	for (size_t i = 0; i < useful_size; i += 2) {
		buf[i+1] = ((int16_t*)buf)[2*i];
		buf[i] = ((int16_t*)buf)[2*i+2];
	}
	useful_size >>= 1;
}

void sink_task(void *param) {
	ESP_LOGD(TAG, "Starting SINK task");

	i2s_chan_handle_t tx_handle;
	/* Get the default channel configuration by helper macro.
	 * This helper macro is defined in 'i2s_common.h' and shared by all the i2s communication mode.
	 * It can help to specify the I2S role, and port id */
	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	/* Allocate a new tx channel and get the handle of this channel */
	i2s_new_channel(&chan_cfg, &tx_handle, NULL);
	/* Setting the configurations, the slot configuration and clock configuration can be generated by the macros
	 * These two helper macros is defined in 'i2s_std.h' which can only be used in STD mode.
	 * They can help to specify the slot and clock configurations for initialization or updating */
	i2s_std_config_t std_cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),  /* 48kHz mono = 48kHz clock */
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
		.gpio_cfg = {
			.mclk = I2S_GPIO_UNUSED,
			.bclk = GPIO_NUM_4,
			.ws = GPIO_NUM_5,
			.dout = GPIO_NUM_18,
			.din = I2S_GPIO_UNUSED,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv = false,
			},
		},
	};
	std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
	/* Initialize the channel */
	i2s_channel_init_std_mode(tx_handle, &std_cfg);

	/* Before write data, start the tx channel first */
	i2s_channel_enable(tx_handle);

	struct timeval t0, t1;
	gettimeofday(&t0, 0);
	uint64_t bytes_written_from_start = 0;

	size_t i = 0;
	while (1) {
		xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
		/* start gathering stats when first sample is actually received */
		if (bytes_written_from_start == 0 && useful_size > 0)
			gettimeofday(&t0, 0);

		//ESP_LOGD(TAG, "US: %d ## PrS ck %x", useful_size, checksum((uint8_t *)buf, useful_size));
		audio_sbramangle_mono_data();
		//ESP_LOGD(TAG, "US: %d ## PoS ck %x", useful_size, checksum((uint8_t *)buf, useful_size));

		size_t bytes_written, bytes_written_total;
		bytes_written_total = 0;
		while (bytes_written_total < useful_size) {
			//ESP_LOGD(TAG, "x");
			i2s_channel_write(tx_handle,
			                  (char *) buf + bytes_written_total,
			                  2 * useful_size - bytes_written_total,
			                  &bytes_written, 10000);
			bytes_written_total += bytes_written;
		}
		xTaskNotify(signals.decoder, 0, eNoAction);

		/* print some stats */
		bytes_written_from_start += bytes_written_total;
		if (i++ % 100 == 0) {
			gettimeofday(&t1, 0);
			time_t elapsed_sec = ((t1.tv_sec - t0.tv_sec) * 1000000 + t1.tv_usec - t0.tv_usec) / 1000 / 1000;
			if (elapsed_sec > 0) {
				uint64_t bytes_per_second = bytes_written_from_start / (uint64_t)elapsed_sec;
				ESP_LOGD(TAG, "Total samples: %llu, total seconds: %lld, Samples per second: %llu",
				         bytes_written_from_start, elapsed_sec, bytes_per_second);
			}
		}
	}

	/* If the configurations of slot or clock need to be updated,
	 * stop the channel first and then update it */
	// i2s_channel_disable(tx_handle);
	// std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO; // Default is stereo
	// i2s_channel_reconfig_std_slot(tx_handle, &std_cfg.slot_cfg);
	// std_cfg.clk_cfg.sample_rate_hz = 96000;
	// i2s_channel_reconfig_std_clock(tx_handle, &std_cfg.clk_cfg);

	/* Have to stop the channel before deleting it */
	i2s_channel_disable(tx_handle);
	/* If the handle is not needed any more, delete it to release the channel resources */
	i2s_del_channel(tx_handle);
}

_Noreturn void source_task(void* param) {
	RingbufHandle_t rb = (RingbufHandle_t)param;

#ifndef SOURCE_TASK_EMBEDDED_DATA
#ifndef STREAM_EMBEDDED_DATA
	while (1) {
		if (wifi_init_sta())
			fetch_radio(rb);
	}
#else  // STREAM_EMBEDDED_DATA
	while (1)
		stream_embedded_data(rb);
#endif  // STREAM_EMBEDDED_DATA
#endif  // SOURCE_TASK_EMBEDDED_DATA
}


StaticRingbuffer_t mp3_ringbuf;
uint8_t mp3_ringbuf_backing_storage[MP3_RINGBUF_SIZE];

void app_main() {
	esp_log_level_set("*", ESP_LOG_INFO);

	// Initialize NVS, needed for Wi-Fi I think? TODO check what this is for...
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

#ifndef SOURCE_TASK_EMBEDDED_DATA
	RingbufHandle_t rb = xRingbufferCreateStatic(MP3_RINGBUF_SIZE,
												 RINGBUF_TYPE_BYTEBUF,
												 mp3_ringbuf_backing_storage,
												 &mp3_ringbuf);
#else
	void *rb = NULL;
#endif

	BaseType_t result;

	result = xTaskCreate(sink_task, "SINK", SINK_STACK_SIZE, NULL,
	                     configMAX_PRIORITIES - 1, &signals.sink);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Could not create task SINK");
		while (1);
	}

	/* pass the ring buffer as a void*, (ab)using the implementation detail that RingbufHandle_t is a pointer type */
	result = xTaskCreate(decoder_task, "DECODER", DECODER_STACK_SIZE, (void *)rb,
	                     configMAX_PRIORITIES - 2, &signals.decoder);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Could not create task DECODER");
		while (1);
	}

	// maestro attacchi!
	xTaskNotify(signals.decoder, 0, eNoAction);

	/* pass the ring buffer as a void*, (ab)using the implementation detail that RingbufHandle_t is a pointer type */
	result = xTaskCreate(source_task, "SOURCE", SOURCE_STACK_SIZE, (void *)rb,
	                     configMAX_PRIORITIES - 3, NULL);
	if (result != pdPASS) {
		ESP_LOGE(TAG, "Could not create task SOURCE");
		while (1);
	}
}
