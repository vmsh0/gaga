# Gaga

Gaga is an ESP32 web radio client with the following characteristics:

- I made this to retrofit a Soviet subscriber radio unit "Donbass 306" (more on this below) and turn
  it into a web radio client
- I made this for fun: when I got the code to work, I cleaned it up the least possible amount I
  could get away with not to feel completely ashamed of myself. This is to say, the code sucks a lot
- It connects via Wi-Fi and fetches an MP3 web radio from a given URL
- The radio must be stereo and 48kHz. The code decodes the MP3 frames using [a slightly modified
  minimp3](https://github.com/chmorgan/minimp3), picks out the left channel, and pushes it out
  through one of the I2S ports
- It doesn't require PSRAM, it (abundantly) fits in the internal DRAM. Meaning you can use it on any
  old crusty cheap ESP32

The functionality is implemented using three different tasks:
- A source tasks, which fetches mp3 frames from the web radio using the ESP HTTP client. It supports
  TLS. It pushes the frames to the decoder task using an ESP-IDF ring buffer, which is a quite
  simple ring buffer implemented over FreeRTOS available in ESP-IDF
- A decoder tasks, which fetches mp3 frames from the ring buffer and decodes them through minimp3.
  Luckily, that library has very solid synchronization capabilities for free-form MP3s: some of the
  web radios I've tried return all kinds of crap at the beginning (ID3 tags, partial frames, etc),
  and minimp3 can basically take it all provided that you show it enough data. To achieve this, the
  decoder task has a special case to accumulate a bunch of mp3 frames before feeding anything to
  minimp3. The library (allegedly) outputs 16-bit signed PCM, which is copied to a small buffer;
  then, a signal is sent to the sink task to read this buffer and the task is blocked until the sink
  task signals it back
- A sink task, which empties the PCM buffer, feeding it directly into IDF-ESP's I2S implementation.
  Due to how I2S is implemented with DMA, some "sbramangling" of the data is required first: the
  samples need to be reordered. It is at this stage that, while I'm linearly going through all the
  samples, I'm also filtering the right channel out (the radio I'm retrofitting only has a single
  speaker; I'm not downmixing left and right because ignoring right is easier -- do you hate this?
  open a PR :)

The result is pretty solid: the implementation recovers from connection hickups... and that's
basically it. A web radio client doesn't do that much :)

## Donbass 306 and subscriber radio
In the URSS, they used to have this "subscriber radio" thing, which was basically cable TV, but for
radio. Each building (including commercial buildings, factories, etc) had a subscriber radio point
of presence, which distributed the radio signal to each particular unit (apartments, shops, offices,
etc.). I'm not sure how the signal was distributed to these points of presence, but the signal was
distributed to the single radio units (e.g. the Donbass 306 radio) as a simple power signal, driving
the speaker directly after going through a small transformer and a potentiometer.

There was usually a local mix (might be city or region level, who knows), and the higher (e.g.
Soviet Ukraine, or the URSS central governemnt) or lower (e.g. city or building administration)
levels of government also had overrides to transmit, replacing the normal programming. Due to the
signal being unencrypted and trivial to record, it is said that there was also a very active bootleg
market, especially for adult entertainment transmissions.

## Building the project
Building is extremely easy:

- Have ESP-IDF (the ESP32 SDK) set up
- Clone the project, change to its directory, give `idf.py build`
- To flash it, it's the usual `idf.py flash`

You probably want to change which web radio this project receives (although I do recommend you
listen to some [Радіо Культура](http://www.nrcu.gov.ua/3channel_about), with a music-filled
programming about Ukranian culture). To do that, you can either change the `STREAMING_RADIO_URL`
definition in main/streaming.h, or you can define explicitly define `STREAMING_RADIO_URL` during
building (the header file will pick up the external definition). Keep in mind that it must be a
stereo 48kHz web radio (or you must also change some other stuff, especially in the sink task).

### Useful stuff

- The `i2s` file contains some notes on the ESP-IDF I2S modules. I suggest you go through the
  official docs first, as otherwise you probably won't understand what any of that is about
- The `offline` directory contains a very small mp3 decoder using minimp3, which I have extensively
  used during the implementation of this project to convince myself that minimp3 indeed does work
- The `parse_a_dump.py` script can take the console output of your ESP32 and extract any hex dumps
  printed using `ESP_LOG_BUFFER_HEX_LEVEL`. I have used this to grab MP3 frames from the ESP32 and
  decode them on my computer, to check that the HTTP client and IPC between source and decoder
  worked correctly

## Contributing to the project
Want to contribute? Here's a list of things that would be nice to have:

- Move all the various flags I have interspersed the code with to Kconfig
- Re-check all the stack & buffer sizes, to make sure they are 1) sane for the current
  implementation 2) adaptable to different parameters (e.g. more audio channels, different sampling
  rate, etc.)
- Move each task to its own compilation unit
- Generally clean up the code
- Automatically reconfigure I2S for whatever kind of signal we get from the web radio
- Properly downmix stereo to mono
- Support on-the-fly reconfiguration, perhaps with an HTTP control panel or a Bluetooth thingy

It would also be nice to have more information on URSS subscriber radio, both technical and
cultural :)

I am also completely open to considering any PR whatsoever: this project does not have a clearly
defined direction at all.

## Similar projects
I did this for fun, fully aware that other people have made similar attempts which have turned out
better or simply received more attentions (and thus features).

Some of the ones I found are:

- [ESP32-Radio](https://github.com/Edzelf/ESP32-Radio), pretty cool project with display support, a
  web interface for configuration, USB stick support, some DSP. Also provides some open hardware!
- [ESP-MiniWebRadio](https://github.com/schreibfaul1/ESP32-MiniWebRadio), all of the above plus
  radio search engine support. Very active project. Requires PSRAM

It might be of interest to some that these are both PlatformIO projects, while mine is plain
"ESP-IDF flavoured" FreeRTOS.

