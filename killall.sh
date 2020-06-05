#!/bin/bash


#for pid in `ls /tmp/zsy/*.pid` 
#do
#	echo $pid
#	kill -9 `cat $pid`
#done


ps aux | grep a.sh | awk '{print $2}' | while read pid
do
	echo "kill pid:" $pid
	kill -9 $pid
done

ps aux | grep zns | awk '{print $2}' | while read pid
do
	echo "kill pid:" $pid
	kill -9 $pid
done

