#!/bin/sh

# example of how to encode one video to DSV2 faster via parallel processing.
# proper displaying of the resulting DSV file requires a video player capable of decoding BC2 color.

npr=$(nproc) # num processes

dsv_executable="./dsv2"
video="source.mkv"
outputfile="saved.dsv"
ffmpeg_cmd="ffmpeg -loglevel quiet -nostats -hide_banner -y"
gop="120"
qp="70"
chunk_per_gop="1"
encode_args="-y4m=1 -colorspace=5 -gop=$gop -qp=$qp -rc_mode=0"

dimension_string="$(ffmpeg -i ${video} 2>&1 | perl -lane 'print $1 if /(\d+x\d+)/' | sed -r 's/[x]+/ /g')"

framerate=$(echo "scale=8; $(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of default=noprint_wrappers=1:nokey=1 ${video})" | bc)
int_framerate=$(printf "%.0f" "$framerate")

let chunk=30 # default frame chunk size if chunk_per_gop is "0"
if [[ $chunk_per_gop -ne 0 ]]; then
    chunk=$gop
fi

start=0 # start time

# internal, used to tell when the input stream has ended
hit_end=0

# enc_chunk: 
#    encodes a chunk of already encoded video by encoding the frames between two time points (start of chunk and end of chunk)
# args = id
enc_chunk() {
	starttime=$(printf '%.6f' "$(echo "$1 / $framerate" | bc -l)")
    ((end=start+chunk))
	endtime=$(printf '%.6f' "$(echo "$end / $framerate" | bc -l)")
			
	countA=$(ffmpeg -t $starttime -i $video -hide_banner -nostats -vcodec copy -y -f rawvideo /dev/null 2>&1 | grep frame | awk '{print $2}')
	countB=$(ffmpeg -t $endtime -i $video -hide_banner -nostats -vcodec copy -y -f rawvideo /dev/null 2>&1 | grep frame | awk '{print $2}')
    ((nframes=countB-countA))
	startflag=""
	if [ "$(echo "$starttime > 0" | bc)" -eq 1 ]; then
		startflag="-ss ${starttime}" # for some reason ffmpeg didnt like -ss 0.00000 so just omit it if the start time is zero.
	fi
	cmd="${ffmpeg_cmd} -an ${startflag} -i ${video} -pix_fmt rgb24 -f rawvideo -frames:v ${nframes} - | rgb2bc2 $dimension_string $int_framerate 2 0 1 0 - - | ${dsv_executable} e -y -inp=- -out=savedsub${i}.dsv ${encode_args} -nfr=${nframes} -noeos=1"
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

