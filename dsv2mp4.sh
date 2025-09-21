#!/bin/sh

die () {
    echo >&2 "$@"
    exit 1
}

if [[ "$#" -eq 0 ]]; then
	die "usage: dsv2mp4 video.dsv (optional: output.mp4)"
fi
if [[ "$#" -eq 2 ]]; then
	output=$2	
else
	file="$1"
	output=${file%.dsv}.mp4
fi

dsv2 d -y -inp=$1 -y4m=1 -drawinfo=0 | ffmpeg -loglevel warning -hide_banner -i pipe: -c:v libx264 -crf 10 -preset fast $output
