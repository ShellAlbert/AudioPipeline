#!/bin/bash

#1.start record app.
#2.start playback app.

while true
do
    #recording from i2s1 to /tmp/zsy.noise fifo.
    arecord -D plughw:tegrasndt186ref,1 -r 32000 -f S32_LE -c 2 -t raw > /tmp/zsy.noise &
    sleep 5

    #noise suppression.

    #playback from /tmp/zsy.clean to i2s0.
    aplay -D plughw:tegrasndt186ref,0  -r 32000 -f S32_LE -c 2 -t raw < /tmp/zsy.clean
    sleep 5
done

#the end of file.



