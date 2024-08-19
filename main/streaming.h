#include <sys/cdefs.h>

#ifndef _H_STREAMING_

#include <freertos/ringbuf.h>

/* chunk size to put in the ring buffer */
#define STREAMING_FETCH_CHUNK_SIZE 1024

#ifndef STREAMING_USER_AGENT
	#define STREAMING_USER_AGENT "RadioGaga/0.1 (bestov.io -- Slava Ukraini from Italy)"
#endif

#ifndef STREAMING_RADIO_URL
	#define STREAMING_RADIO_URL "https://radio3.ukr.radio/ur3-mp3-m"
#endif

/* total chunks ever read from streaming module.
 * with 128kbit/s MP3 uint32_t lasts ~1 year.
 * TODO synchronization */
extern volatile uint32_t streaming_total_chunks_read;

/* start fetching chunks and put the in given ring buffer */
void fetch_radio(RingbufHandle_t);

/* stream embedded data to the given ring buffer */ _Noreturn
void stream_embedded_data(RingbufHandle_t);

/* initialize The Internet(TM) */
bool wifi_init_sta();

#endif
