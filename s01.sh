#!/bin/bash

#1.config i2s0&i2s1.
#2.make all fifos.
#3.start json nc.
#4.start uart nc.

#amixer -c tegrasndt186ref sset "ADMAIF2 Mux" I2S2
#amixer -c tegrasndt186ref sset "I2S1 Mux" ADMAIF1

#volume control enabled.
amixer -c tegrasndt186ref sset "ADMAIF2 Mux"  "I2S2"
amixer -c tegrasndt186ref sset "I2S1 Mux"  "MVC1"
amixer -c tegrasndt186ref sset "MVC1 Mux" "ADMAIF1"
amixer -c tegrasndt186ref cset name="MVC1 Vol"  13000
#amixer -c tegrasndt186ref cset name="MVC1 Vol"  9000
#amixer -c tegrasndt186ref cset name="MVC1 Vol"  16000

#i2s format.
amixer -c tegrasndt186ref cset name='I2S1 codec frame mode' 'i2s'
amixer -c tegrasndt186ref cset name='I2S2 codec frame mode' 'i2s'

#master mode.
amixer -c tegrasndt186ref cset name='I2S1 codec master mode' 'cbs-cfs'
amixer -c tegrasndt186ref cset name='I2S2 codec master mode' 'cbm-cfm'

#16bit/32bit
amixer -c tegrasndt186ref cset name='I2S1 codec bit format' '32'
amixer -c tegrasndt186ref cset name='I2S2 codec bit format' '32'

#input bit format.
amixer -c tegrasndt186ref cset name='I2S1 input bit format' '32'
amixer -c tegrasndt186ref cset name='I2S2 input bit format' '32'

#i2s0 (play):plughw:tegrasndt186ref,0
#i2s1 (record):plughw:tegrasndt186ref,1
#i2s1 --> fifo --> i2s0
mkfifo /tmp/zsy.noise
chmod 777 /tmp/zsy.noise

mkfifo /tmp/zsy.clean
chmod 777 /tmp/zsy.clean

mkfifo /tmp/zsy.opus
chmod 777 /tmp/zsy.opus

#json ctrl fifo.
mkfifo /tmp/zsy.json.rx
chmod 777 /tmp/zsy.json.rx

mkfifo /tmp/zsy.json.tx
chmod 777 /tmp/zsy.json.tx

nc -l 6802 -k > /tmp/zsy.json.rx  < /tmp/zsy.json.tx &

#tcp<->uart.
stty -F /dev/ttyTHS2 115200 raw -echo -echoe -echok
#-k:Forces nc to stay listening for another connection after its current connection is completed.
nc -l 6804 -k > /dev/ttyTHS2 < /dev/ttyTHS2 &

exit 0
#the end of file.



