#!/bin/bash

#all temporary files are located in /tmp/zsy directory.
TMP_DIR=/tmp/zsy
mkdir $TMP_DIR

#i2s1 -> zsy.noise -> zns -> zsy.clean -> i2s0.
#                         -> zsy.opus  -> APP.
mkfifo $TMP_DIR/zsy.noise
mkfifo $TMP_DIR/zsy.clean
mkfifo $TMP_DIR/zsy.opus

#zns -> zsy.json.tx -> nc -> APP.
#zns <- zsy.json.rx <- nc <- APP.
mkfifo $TMP_DIR/zsy.json.tx
mkfifo $TMP_DIR/zsy.json.rx

#zns -> zsy.opus -> nc ->APP.
mkfifo $TMP_DIR/zsy.opus

#config i2s0 & i2s1.
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

#kill all first.
for pid in `ls $TMP_DIR/*.pid` 
do
	kill -9 `cat $pid`
done
sleep 5

#define pid variables.
pid_rec=
pid_play=
pid_opus=
pid_json=
pid_uart=
pid_zns=

#loop to monitor each shell,restart if detected fails.
while true
do
	#check arecord pid.
	if [ ! $pid_rec ];then
		arecord -D plughw:tegrasndt186ref,1 -r 32000 -f S32_LE -c 2 -t raw > $TMP_DIR/zsy.noise &
		pid_rec=$!
		echo $pid_rec > /tmp/zsy/rec.pid
		echo "restart rec pid okay:" $pid_rec
	else
		kill -0 $pid_rec
		if [ $? -eq 0 ];then
			echo "rec pid okay:" $pid_rec
		else
			echo "rec pid error"
			pid_rect=
		fi
	fi
	
	#check aplay pid.
	if [ ! $pid_play ];then
		aplay -D plughw:tegrasndt186ref,0 -r 32000 -f S32_LE -c 2 -t raw < $TMP_DIR/zsy.clean &
		#cat < $TMP_DIR/zsy.clean > /dev/null &
		pid_play=$!
		echo $pid_play > /tmp/zsy/play.pid
		echo "restart play pid okay:" $pid_play
	else
		kill -0 $pid_play
		if [ $? -eq 0 ];then
			echo "play pid okay:" $pid_play
		else
			echo "play pid error"
			pid_play=
		fi
	fi

	#check json pid.
	if [ ! $pid_json ];then
		nc -l 6802 -k > $TMP_DIR/zsy.json.rx  < $TMP_DIR/zsy.json.tx &
		pid_json=$!
		echo $pid_json > /tmp/zsy/json.pid
		echo "restart json pid okay:" $pid_json
	else
		kill -0 $pid_json
		if [ $? -eq 0 ];then
			echo "json pid okay:" $pid_json
		else
			echo "json pid error"
			pid_json=
		fi
	fi

	#check uart pid.
	if [ ! $pid_uart ];then
		nc -l 6804 -k > /dev/ttyTHS2 < /dev/ttyTHS2 &
		pid_uart=$!
		echo $pid_uart > /tmp/zsy/uart.pid
		echo "restart uart pid okay:" $pid_uart
	else
		kill -0 $pid_uart
		if [ $? -eq 0 ];then
			echo "uart pid okay:" $pid_uart
		else
			echo "uart pid error"
			pid_uart=
		fi
	fi

	#check zns pid.
	#if [ ! $pid_zns ];then
	#	./zns &
	#	pid_zns=$!
	#	echo $pid_zns > /tmp/zsy/zns.pid
	#	echo "restart zns pid okay:" $pid_zns
	#else
	#	kill -0 $pid_zns
	#	if [ $? -eq 0 ];then
	#		echo "zns pid okay:" $pid_zns
	#	else
	#		echo "zns pid error"
	#		pid_zns=
	#	fi
	#fi
	sleep 5
done
exit 0
