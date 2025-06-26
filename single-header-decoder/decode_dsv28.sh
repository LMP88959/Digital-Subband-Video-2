#!/bin/sh

# usage: ./decode_dsv28.sh video.dsv

# compile if executable doesn't exist
if [[ ! -f "dsv28dec" ]]; then
    cc -O3 -o dsv28dec d28_dec_main.c
fi

./dsv28dec -y -v -inp=$1 -out=decom.y4m -y4m=1
ffmpeg -loglevel quiet -nostats -hide_banner -y -i decom.y4m -c:v libx264 -qp 0 -crf 10 -preset fast test.mp4
