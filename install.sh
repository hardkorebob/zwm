#!/bin/sh
CONF=$HOME/.config/swm
DIR=/usr/local/bin
cc -O2 -o swm swm.c -lX11 -Wall -Wextra -pedantic 
strip -s swm
mkdir $CONF 
cp config $CONF
chmod +x swmctl swm-session
sudo cp swmctl swm-session swm $DIR
sudo cp swm.desktop /usr/share/xsessions
