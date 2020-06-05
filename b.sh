#!/bin/bash

#kill all related PID.
kill_arecord()
{
	ps aux | grep "arecord -D plughw:tegrasndt186ref,1" | awk '{print $2}' | while read pid
	do
		echo "try to kill pid:" $pid
		kill -9 $pid
	done
}
kill_aplay()
{
        ps aux | grep "aplay -D plughw:tegrasndt186ref,0" | awk '{print $2}' | while read pid
        do
                echo "try to kill pid:" $pid
                kill -9 $pid
        done
}
kill_json()
{
        ps aux | grep "nc -l 6802 -k" | awk '{print $2}' | while read pid
        do
                echo "try to kill pid:" $pid
                kill -9 $pid
        done
}
kill_uart()
{
        ps aux | grep "nc -l 6804 -k" | awk '{print $2}' | while read pid
        do
                echo "try to kill pid:" $pid
                kill -9 $pid
        done
}
kill_zns()
{
        ps aux | grep "./zns" | awk '{print $2}' | while read pid
        do
                echo "try to kill pid:" $pid
                kill -9 $pid
        done
}

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
kill_arecord
kill_aplay
kill_json
kill_uart
kill_zns

#define pid variables.
pid_rec=
pid_play=
pid_opus=
pid_json=
pid_uart=
pid_zns=

#loop to monitor each shell,restart if detected fails.
iChkTimes=0
while true
do
	echo "checking times:" $iChkTimes
	let iChkTimes=$iChkTimes+1
	sleep 6 

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
			pid_rec=
			kill_arecord
		fi
	fi
	
	#check aplay pid.
	if [ ! $pid_play ];then
		#aplay -D plughw:tegrasndt186ref,0 -r 32000 -f S32_LE -c 2 -t raw < $TMP_DIR/zsy.clean &
		aplay -D plughw:tegrasndt186ref,0 -r 48000 -f S16_LE -c 2 -t raw < $TMP_DIR/zsy.clean &
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
			kill_aplay
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
			kill_json
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
			kill_uart
		fi
	fi

	#check zns pid.
	if [ ! $pid_zns ];then
		./zns &
		pid_zns=$!
		echo $pid_zns > /tmp/zsy/zns.pid
		echo "restart zns pid okay:" $pid_zns
	else
		kill -0 $pid_zns
		if [ $? -eq 0 ];then
			echo "zns pid okay:" $pid_zns
		else
			echo "zns pid error"
			pid_zns=
			kill_zns
		fi
	fi
done

exit 0
#the end of file.
