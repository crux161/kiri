#!/bin/sh

export URL="https://download.blender.org/peach/bigbuckbunny_movies/big_buck_bunny_1080p_h264.mov"
export FILE="big_buck_bunny_1080p_h264.mov"

# check if https://download.blender.org/peach/bigbuckbunny_movies/big_buck_bunny_1080p_h264.mov
# exists locally, if not -- then download the big_buck_bunny video file

if [ ! -f $FILE ]; then
	echo "$FILE not found, donwloading..."
	wget $URL
else
	echo "$FILE found..."
fi
