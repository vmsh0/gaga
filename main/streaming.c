#include <sys/cdefs.h>
#include <string.h>
#include <freertos/ringbuf.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_tls.h>
#include <esp_http_client.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include "streaming.h"

#include "checksum.h"

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

static const char *TAG = "a_streaming";

static int s_retry_num = 0;

static wifi_ap_record_t records[16];

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		//ESP_LOGI(TAG, "Will launch scan");
		//esp_wifi_scan_start(NULL, false);
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
		ESP_LOGI(TAG, "Scan done");
		uint16_t records_number = sizeof(records)/sizeof(records[0]);
		esp_wifi_scan_get_ap_records(&records_number, records);
		ESP_LOGI(TAG, "Found %d APs", records_number);
		for (size_t i = 0; i < records_number; i++) {
			ESP_LOGI(TAG, "AP Record %s RSSI:%d", records[i].ssid, records[i].rssi);
		}
		while (1);
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		} else {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG, "connect to the AP fail");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

bool wifi_init_sta() {
	esp_log_level_set("wifi", ESP_LOG_DEBUG);

	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
	                                                    ESP_EVENT_ANY_ID,
	                                                    &event_handler,
	                                                    NULL,
	                                                    &instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
	                                                    IP_EVENT_STA_GOT_IP,
	                                                    &event_handler,
	                                                    NULL,
	                                                    &instance_got_ip));

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = EXAMPLE_ESP_WIFI_SSID,
			.password = EXAMPLE_ESP_WIFI_PASS,
			/* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
			 * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
			 * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
			 * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
			 */
			.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
			.sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "wifi_init_sta finished.");

	/* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
	 * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
	                                       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
	                                       pdTRUE,
	                                       pdFALSE,
	                                       portMAX_DELAY);

	/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
	 * happened. */
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
		         EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
		return true;
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
		         EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
		return false;
	} else {
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
		return false;
	}
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
	switch (evt->event_id) {
		case HTTP_EVENT_ERROR:
			ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
			break;
		case HTTP_EVENT_ON_CONNECTED:
			ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
			break;
		case HTTP_EVENT_HEADER_SENT:
			ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
			break;
		case HTTP_EVENT_ON_HEADER:
			ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
			break;
		case HTTP_EVENT_ON_DATA:
			ESP_LOGV(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
			break;
		case HTTP_EVENT_ON_FINISH:
			ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
			break;
		case HTTP_EVENT_DISCONNECTED:
			ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
			int mbedtls_err = 0;
			esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t) evt->data, &mbedtls_err, NULL);
			if (err != 0) {
				ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
				ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
			}
			break;
		case HTTP_EVENT_REDIRECT:
			ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
			break;
	}
	return ESP_OK;
}

char chunk_buf[STREAMING_FETCH_CHUNK_SIZE];
volatile uint32_t streaming_total_chunks_read = 0;

void fetch_radio(RingbufHandle_t rb) {
	esp_http_client_config_t config = {
		.user_agent = STREAMING_USER_AGENT,
		.url = STREAMING_RADIO_URL,
		.event_handler = _http_event_handler,
		.crt_bundle_attach = esp_crt_bundle_attach,
	};
	esp_http_client_handle_t cl = esp_http_client_init(&config);
	esp_err_t err = esp_http_client_open(cl, 0);
	if (err == ESP_OK) {
		int64_t fetch_result;
		do {
			fetch_result = esp_http_client_fetch_headers(cl);
		} while (fetch_result == -ESP_ERR_HTTP_EAGAIN);

		ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %"PRIu64", chunked=%d",
		         esp_http_client_get_status_code(cl),
		         esp_http_client_get_content_length(cl),
				 esp_http_client_is_chunked_response(cl));

		int ret;
		do {
			/* fill the buffer */
			size_t bytes_read_for_chunk = 0;
			do {
				assert(bytes_read_for_chunk < STREAMING_FETCH_CHUNK_SIZE);
				ret = esp_http_client_read(cl,
										   chunk_buf + bytes_read_for_chunk,
										   STREAMING_FETCH_CHUNK_SIZE - bytes_read_for_chunk);
				if (ret >= 0) {
					bytes_read_for_chunk += ret;
					streaming_total_chunks_read++;
				}
			} while (bytes_read_for_chunk != STREAMING_FETCH_CHUNK_SIZE && ret != ESP_FAIL);

			ESP_LOGD(TAG, "Read chunk, status %d", ret);

			/* if the read was successful, put the chunk in the ring buffer. we ignore the return value of
			 * xRingbufferSend, because if the send fails with MAX_DELAY there's not much we can do anyway. the docs
			 * are very vague about the reasons why it might fail. */
			if (bytes_read_for_chunk == STREAMING_FETCH_CHUNK_SIZE && ret != ESP_FAIL)
				xRingbufferSend(rb, chunk_buf, STREAMING_FETCH_CHUNK_SIZE, portMAX_DELAY);
		} while (ret != ESP_FAIL);
	} else {
		ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
	}
}


#include "data.h"

_Noreturn void stream_embedded_data(RingbufHandle_t rb) {
	/* get MP3 data pointer */
	const uint8_t *audio_data = audio_data_start;
	size_t audio_data_len = audio_data_end - audio_data_start;
	size_t cur_pos = 0;

	do {
		/* fill the buffer */
		size_t amt_to_read = STREAMING_FETCH_CHUNK_SIZE;
		if (amt_to_read > audio_data_len - cur_pos)
			amt_to_read = audio_data_len - cur_pos;

		memcpy(chunk_buf, audio_data + cur_pos, amt_to_read);
		memset(chunk_buf + amt_to_read, 0, STREAMING_FETCH_CHUNK_SIZE - amt_to_read);

		cur_pos += amt_to_read;
		if (cur_pos == audio_data_len)
			cur_pos = 0;

		xRingbufferSend(rb, chunk_buf, STREAMING_FETCH_CHUNK_SIZE, portMAX_DELAY);
		//ESP_LOGD(TAG, "Sent %d bytes with ck %x", STREAMING_FETCH_CHUNK_SIZE, checksum((uint8_t*)chunk_buf, STREAMING_FETCH_CHUNK_SIZE));
	} while (1);
}
