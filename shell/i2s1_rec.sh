#!/bin/bash

#amixer -c tegrasndt186ref sset "ADMAIF2 Mux" I2S2
#arecord -D hw:tegrasndt186ref,1 -r 32000 -f S32_LE -c 2 -t raw -d 30  cap.32k.32bit.pcm.raw

#because PCM1754 is MSB required,so FPGA outputs MSB format.
#so here we use plughw to convert LSB to MSB.
arecord -D hw:tegrasndt186ref,1 -r 32000 -f S32_LE -c 2 -t raw -d 20  cap.32k.32bit.pcm.raw
