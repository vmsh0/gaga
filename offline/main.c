#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_NONSTANDARD_BUT_LOGICAL
#define MINIMP3_IMPLEMENTATION
#include "../main/minimp3.h"
#include "data.h"

#define AUDIO_BUF_SIZE MINIMP3_MAX_SAMPLES_PER_FRAME

mp3d_sample_t buf[AUDIO_BUF_SIZE];
size_t useful_size = 0;  /* used by the source to signal available bytes, used by the sink to signal used bytes */

int main() {
  printf("First 16 bytes:\n%02x %02x %02x %02x %02x %02x %02x %02x\n%02x %02x %02x %02x %02x %02x %02x %02x\n",
         audio_data[0],  audio_data[1],  audio_data[2],  audio_data[3],  audio_data[4],  audio_data[5],  audio_data[6],  audio_data[7],
         audio_data[8],  audio_data[9],  audio_data[10], audio_data[11], audio_data[12], audio_data[13], audio_data[14], audio_data[15]);

	mp3dec_frame_info_t info;
	size_t cur_pos = 0;
	size_t samples, retries;

  /* init MP3 decoder */
	mp3dec_t mp3d;
	mp3dec_init(&mp3d);

  /* save output file */
  FILE *f = fopen("out.wav", "w");

	while (1) {
    printf(" * begin cycle\n");

		samples = 0;
		retries = 5;

		while (samples == 0 && cur_pos < audio_data_len && --retries) {
      info.frame_bytes = 0;
			samples = mp3dec_decode_frame(&mp3d, audio_data + cur_pos, audio_data_len - cur_pos, buf, &info);
      cur_pos += info.frame_bytes;
      printf(" ** Samples: %lu\n", samples);
      if (info.frame_bytes > 0)
        printf(" ** Advancing by %d bytes\n", info.frame_bytes);
		}

		printf(" ** Tries: %lu, Used bytes: %d\n", 5 - retries, info.frame_bytes);
    printf(" ** Pos: %09lu / %d\n", cur_pos, audio_data_len);

		if (!retries) {
			printf(" ** Failed, resetting source task\n");
			cur_pos = 0;  /* reset */
		} else {  
      printf(" ** Decoded audio info: channels=%d, bitrate=%d kbps, hz=%d, layer=%d\n", info.channels, info.bitrate_kbps, info.hz, info.layer);
      if (f != NULL)
        fwrite(buf, 1, samples * sizeof(int16_t) * 2, f);
			if (cur_pos >= audio_data_len) {
				cur_pos = 0;
        if (f != NULL) {
          fclose(f);
          f = NULL;
        }
        printf(" ** Resetting buffer\n");
        return 0;
      }
		}

    // sleep(5);
    printf(" * end cycle\n");
	}

}

