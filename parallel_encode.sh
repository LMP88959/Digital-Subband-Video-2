#!/bin/sh

# example of how to encode one video to DSV2 faster via parallel processing.

npr=8 # num processes

dsv_executable="./dsv2"
video="source.yuv"
vwidth="1280"
vheight="720"
framerate_num="30000"
framerate_den="1001"
gop="60"
qp="50"
chunk_per_gop="1" # gop-sized chunks? otherwise user defined chunks
encode_args="-w=$vwidth -h=$vheight -fps_num=$framerate_num -fps_den=$framerate_den -gop=$gop -qp=$qp -rc_mode=0"

let chunk=30 # default frame chunk size if chunk_per_gop is "0"
if [[ $chunk_per_gop -ne 0 ]]; then
    chunk=$gop
    encode_args="$encode_args -varint=0"
fi

start=0 # start frame number

# internal, used to tell when the input stream has ended
hit_end=0

# mp_encode_sub: 
#    encodes a subset of the video by splitting the subset into chunks and encoding each chunk in parallel
# args = id
mp_encode_sub() {
    subcatstring=""
    id=$1
    for i in $(seq 1 ${npr});
    do
        cmd="${dsv_executable} e -y -inp=${video} -out=savedsub${i}.dsv ${encode_args} -sfr=${start} -nfr=${chunk} -noeos=1"
        #echo ${cmd}
        subcatstring="${subcatstring} savedsub${i}.dsv"
        ((start=start+chunk))
        $cmd &
    done
    # wait for jobs to end, collect their statuses
    for job in `jobs -p`; do
        wait "$job"
        status=$?
        #echo $status
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
   # echo "catstring is ${catstring}"
    cat ${catstring} > saved.dsv
    rm ${catstring}
}

# run the multiprocess encode
time mp_encode

exit 0

