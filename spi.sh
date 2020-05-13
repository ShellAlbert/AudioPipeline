#!/bin/bash

#tx 32bit register address.
#sudo spidev_test  -D /dev/spidev3.0  -H -p "\x80\x00\x82\x00"
#tx 32bit invalid data to generate readback clk.
#sudo spidev_test  -D /dev/spidev3.0  -H -p "\x12\x34\x56\x78"
#read 0x82 register.
sudo spidev_test  -D /dev/spidev3.0  -H -p "\x80\x00\x82\x00\x12\x34\x56\x78"
#volume control.
sudo spidev_test  -D /dev/spidev3.0  -H -p "\x80\x00\x00\x00\x00\x00\x00\x64"


