#!/bin/bash
#amixer -c tegrasndt186ref sset "ADMAIF2 Mux" I2S2
#amixer -c tegrasndt186ref sset "I2S1 Mux" ADMAIF1

#volume control enabled.
amixer -c tegrasndt186ref sset "ADMAIF2 Mux"  "I2S2"
amixer -c tegrasndt186ref sset "I2S1 Mux"  "MVC1"
amixer -c tegrasndt186ref sset "MVC1 Mux" "ADMAIF1"
amixer -c tegrasndt186ref cset name="MVC1 Vol"  10500
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



