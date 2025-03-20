#!/bin/sh
#
export FILE="./bunny.mp4"

if [ -f $FILE ] && [ -s $FILE ]; then
	echo "bunny.mp4 found..."
else
	wget --output-document bunny.mp4 http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4
fi
