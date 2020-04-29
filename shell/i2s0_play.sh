#!/bin/bash

#config=1
config=0

if [ $config -eq 1 ];then
	amixer -c tegrasndt186ref sset "I2S1 Mux"  "MVC1"
	amixer -c tegrasndt186ref sset "MVC1 Mux" "ADMAIF1"
	amixer -c tegrasndt186ref cset name="MVC1 Vol"  12000
	amixer -c tegrasndt186ref cset name='I2S1 codec bit format' 32
else
	#call this script with root priority.
	#44.1khz,16 bit play okay.
	aplay -D hw:tegrasndt186ref,0 -r 44100 -f S16_LE -c 2 -t raw dream.wav
	#32khz,32bit play okay.
	#aplay -D hw:tegrasndt186ref,0  -r 32000 -f S32_LE -c 2 -t raw 32k.32bit.pcm.raw
	#aplay -D plughw:tegrasndt186ref,0  -r 32000 -f S32_LE -c 2 -t raw 32k.32bit.pcm.raw
fi

