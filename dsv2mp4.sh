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
	
tmpdir="$(mktemp -d)"
trap 'rm -rf -- "$tmpdir"' EXIT

y4mfile="$tmpdir/temp.y4m"

cmd="dsv2 d -y -v -inp=$1 -out=${y4mfile} -y4m=1 -drawinfo=0"
$cmd

cmd="ffmpeg -loglevel quiet -nostats -hide_banner -i ${y4mfile} -c:v libx264 -crf 10 -preset fast $output"
$cmd

rm -rf -- "$tmpdir"
