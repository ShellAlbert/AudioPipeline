#!/bin/bash
for pid in `ls /tmp/zsy/*.pid` 
do
	echo $pid
	kill -9 `cat $pid`
done
