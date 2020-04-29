#!/bin/bash
#record from i2s1 pipeline aplay to i2s0.
#arecord -D hw:tegrasndt186ref,1 -r 32000 -f S32_LE -c 2 -t raw | aplay -D plughw:tegrasndt186ref,0  -r 32000 -f S32_LE -c 2 -t raw
arecord -D plughw:CARD=USBSA,DEV=0 -r 32000 -f S32_LE -c 2 -t raw | aplay -D hw:tegrasndt186ref,0  -r 32000 -f S32_LE -c 2 -t raw
