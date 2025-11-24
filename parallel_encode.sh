#!/bin/sh

# example of how to encode one video to DSV2 faster via parallel processing.
# this version uses a .mp4 input file (could be .mkv or anything else that ffmpeg supports)
#
# this script does not create any temporary files.

npr=8 # num processes

dsv_executable="dsv2" # location of dsv2 executable
video="input.mp4" # name of input file
outputfile="output.dsv" # name of output dsv2 file
gop="60"
qp="50"
chunk_per_gop="1"
encode_args="-y4m=1 -gop=$gop -qp=$qp -rc_mode=0"
ffmpeg_filter_args="" # ex: "-vf scale=640:480" or something

# ---- internal

ffmpeg_cmd="ffmpeg -loglevel quiet -nostats -hide_banner -y"
framerate=$(echo "scale=8; $(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of default=noprint_wrappers=1:nokey=1 ${video})" | bc)

let chunk=30 # default frame chunk size if chunk_per_gop is "0"
if [[ $chunk_per_gop -ne 0 ]]; then
    chunk=$gop
fi

start=0 # start time
hit_end=0 # used to tell when the input stream has ended. the dsv2 encoder returns nonzero if end-of-stream is reached

# enc_chunk: 
#    encodes a chunk of already encoded video by encoding the frames between two time points (start of chunk and end of chunk)
# args = start frame number
enc_chunk() {
	starttime=$(printf '%.6f' "$(echo "$1 / $framerate" | bc -l)")
    ((end=$1+chunk))
	endtime=$(printf '%.6f' "$(echo "$end / $framerate" | bc -l)")
	# get frame number at start and end timestamps
	countA=$(ffmpeg -t $starttime -i $video -nostats -vcodec copy -y -f rawvideo /dev/null 2>&1 | grep frame | awk '{print $2}')
	countB=$(ffmpeg -t $endtime -i $video -nostats -vcodec copy -y -f rawvideo /dev/null 2>&1 | grep frame | awk '{print $2}')
    ((nframes=countB-countA))
	startflag=""
	if [ "$(echo "$starttime > 0" | bc)" -eq 1 ]; then
		startflag="-ss ${starttime}" # for some reason ffmpeg didnt like -ss 0.00000 so just omit it if the start time is zero.
	fi
	cmd="${ffmpeg_cmd} -an ${startflag} -i ${video} ${ffmpeg_filter_args} -f yuv4mpegpipe -frames:v ${nframes} - | ${dsv_executable} e -y -inp=- -out=savedsub${i}.dsv ${encode_args} -nfr=${nframes} -noeos=1"
   # echo ${cmd}
	eval $cmd
}

# mp_encode_sub: 
#    encodes a subset of the video by splitting the subset into chunks and encoding each chunk in parallel
# args = id
mp_encode_sub() {
    subcatstring=""
    id=$1
	pids=()
    for i in $(seq 1 ${npr}); do
        subcatstring="${subcatstring} savedsub${i}.dsv"
		enc_chunk $start &
        ((start=start+chunk))
		lpid=$!
		pids+="$lpid "
        # echo "launched $lpid"
    done
    for pid in ${pids[@]}; do
      	wait ${pid}
      	status=$?
    	((hit_end=hit_end | status))
    done
    #echo "subcatstring is ${subcatstring}"
    cat ${subcatstring} > saved${id}.dsv
    rm ${subcatstring}
}

# mp_encode: 
#    encodes a video by splitting it into groups and waiting until mp_encode_sub reports the stream has ended
# args = none
mp_encode() {
    catstring=""
    id=0
    while true
    do
        hit_end=0 # clear, gets set in mp_encode_sub
        ((id++))
		echo "encoding group ${id}"
        mp_encode_sub $id
        catstring="${catstring} saved${id}.dsv"
       # echo $hit_end
        if [[ $hit_end -ne 0 ]]; then
			echo "potentially hit the end of the stream, exiting"
            break
        fi
    done
    echo "catstring is ${catstring}"
    cat ${catstring} > $outputfile
    rm ${catstring}
}

# run the multiprocess encode
time mp_encode

exit 0
