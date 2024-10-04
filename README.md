# Digital-Subband-Video-2
------

DSV2 is a lossy video codec using wavelets and block-based motion compensation.
It is designed for medium-low to medium-high bitrates.
Comparable to MPEG-4 Part 2 and Part 10 (H.264) (P frames only) in terms of efficiency and quality.

------
## Example comparison (More can be found at the bottom of the README):

minih264:


https://github.com/user-attachments/assets/ee54487a-dfb5-4b75-9557-ba6b66a98d59


DSV2:


https://github.com/user-attachments/assets/7ba5a239-4cc9-4852-bb2f-74cb59e05333

------

## DSV2 Features (refer to PDF in repo for more detail)

- compression using multiresolution subband analysis instead of DCT
   - also known as a wavelet transform
- up to quarter-pixel motion compensation
- 4:1:1, 4:2:0, 4:2:2 (+ UYVY) and 4:4:4 chroma subsampling formats
- adaptive quantization
- in-loop filtering
- intra and inter frames with variable length closed GOP
   - no bidirectional prediction (also known as B-frames). Only forward prediction with previous picture as reference
- only form of entropy coding is interleaved exponential-golomb coding for simplicity

## Improvements and New Features since DSV1

- in-loop filtering after motion compensation
- more adaptive quantization potential
- skip blocks for temporal stability
- new subband filters + support for adaptive subband filtering
- better motion compensation through Expanded Prediction Range Mode (EPRM)
- quarter pixel compensation
- psychovisual optimizations in the codec and encoder design


--- for more detailed information please refer to the informal specification document (DSV2_spec.pdf) in the repository.

## Encoder Features (specific to this implementation of the DSV2 spec)

- single pass average bitrate (ABR) or constant rate factor (CRF) rate control
- more advanced Human Visual System (HVS) based intra block mode determination
- new Human Visual System (HVS) based intra frame adaptive quantization
- more complex scene change detection
- hierarchical motion estimation
- better temporal adaptive quantization
- written to be compatible with C89

--- for more detailed information please refer to the encoder information document (DSV2_encoder.pdf) in the repository.

## Limitations

- no built-in interlacing support
- only 8 bits of depth per component supported
- frame sizes must be divisible by two

This code follows my self-imposed restrictions:

1. Everything must be done in software, no explicit usage of hardware acceleration.
2. No floating point types or literals, everything must be integer only.
3. No 3rd party libraries, only C standard library and OS libraries for window, input, etc.
4. No languages used besides C.
5. No compiler specific features and no SIMD.
6. Single threaded.

## Compiling

### C Compiler

All you need is a C compiler.

In the root directory of the project (with all the .h and .c files):
```bash
cc -O3 -o dsv2 *.c
```

## Running Encoder

Sample output:
```
Envel Graphics DSV v2.0 compliant codec by EMMIR 2024
usage: ./dsv2 e [options]
sample usage: ./dsv2 e -inp=video.yuv -out=compressed.dsv -w=352 -h=288 -fps_num=24 -fps_den=1 -qp=85 -gop=15
------------------------------------------------------------
	-qp : quality percent. If -1 and ABR mode, estimate good qp for desired bitrate. If -1 and CRF mode, default to 85. -1 = default
	      [min = -1, max = 100]
	-effort : encoder effort. 0 = least effort, 10 = most effort. higher value -> better video, slower encoding. default = 10
	      [min = 0, max = 10]
	-w : width of input video. 352 = default
	      [min = 16, max = 16777216]
	-h : height of input video. 288 = default
	      [min = 16, max = 16777216]
	-gop : Group Of Pictures length. 0 = intra frames only, 12 = default
	      [min = 0, max = 2147483647]
	-fmt : chroma subsampling format of input video. 0 = 4:4:4, 1 = 4:2:2, 2 = 4:2:0, 3 = 4:1:1, 4 = 4:2:2 UYVY, 2 = default
	      [min = 0, max = 4]
	-nfr : number of frames to compress. -1 means as many as possible. -1 = default
	      [min = -1, max = 2147483647]
	-sfr : frame number to start compressing at. 0 = default
	      [min = 0, max = 2147483647]
	-fps_num : fps numerator of input video. 30 = default
	      [min = 1, max = 16777216]
	-fps_den : fps denominator of input video. 1 = default
	      [min = 1, max = 16777216]
	-aspect_num : aspect ratio numerator of input video. 1 = default
	      [min = 1, max = 16777216]
	-aspect_den : aspect ratio denominator of input video. 1 = default
	      [min = 1, max = 16777216]
	-ipct : percentage threshold of intra blocks in an inter frame after which it is simply made into an intra frame. 50 = default
	      [min = 0, max = 100]
	-pyrlevels : number of pyramid levels to use in hierarchical motion estimation. 0 means auto-determine. 0 = default
	      [min = 0, max = 5]
	-rc_mode : rate control mode. 0 = single pass average bitrate (ABR), 1 = constant rate factor (CRF). 0 = default
	      [min = 1, max = 0]
	-kbps : ONLY FOR ABR RATE CONTROL: bitrate in kilobits per second. 0 = auto-estimate needed bitrate for desired qp. 0 = default
	      [min = 0, max = 2147483647]
	-maxqstep : max quality step for ABR, absolute quant amount in range [1, 400]. 1 = default (0.25%)
	      [min = 1, max = 400]
	-minqp : minimum quality. 0 = default
	      [min = 0, max = 100]
	-maxqp : maximum quality. 100 = default
	      [min = 0, max = 100]
	-iminqp : minimum quality for intra frames. 20 = default
	      [min = 0, max = 100]
	-stabref : period (in # of frames) to refresh the stability block tracking. 0 = auto-determine. 0 = default
	      [min = 0, max = 2147483647]
	-scd : do scene change detection. 1 = default
	      [min = 0, max = 1]
	-tempaq : do temporal adaptive quantization. If disabled, spatial methods will be used instead. 1 = default
	      [min = 0, max = 1]
	-bszx : override block sizes in the x (horizontal) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32
	      [min = -1, max = 1]
	-bszy : override block sizes in the y (vertical) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32
	      [min = -1, max = 1]
	-scpct : scene change percentage. 55 = default
	      [min = 0, max = 100]
	-skipthresh : skip block threshold. -1 = disable. 0 = default, larger value means more likely to mark a block as skipped.
	      [min = -1, max = 2147483647]
	-varint : intra frames that are created outside of the normal GOP cycle reset the GOP cycle if 1. 1 = default
	      [min = 0, max = 1]
	-psy : enable/disable psychovisual optimizations. 1 = default
	      [min = 0, max = 1]
	-y4m : set to 1 if input is in Y4M format, 0 if raw YUV. 0 = default
	      [min = 0, max = 1]
	-inp= : input file. NOTE: if not specified, defaults to stdin
	-out= : output file. NOTE: if not specified, defaults to stdout
	-y : do not prompt for confirmation when potentially overwriting an existing file
	-l<n> : set logging level to n (0 = none, 1 = error, 2 = warning, 3 = info, 4 = debug/all)
	-v : set verbose
```

## Running Decoder

Sample output:
```
Envel Graphics DSV v2.0 compliant codec by EMMIR 2024
usage: ./dsv2 d [options]
sample usage: ./dsv2 d -inp=video.dsv -out=decompressed.yuv -out420p=1
------------------------------------------------------------
	-out420p : convert video to 4:2:0 chroma subsampling before saving output. 0 = default
	      [min = 0, max = 1]
	-drawinfo : draw debugging information on the decoded frames (bit OR together to get multiple at the same time):
		1 = draw stability info
		2 = draw motion vectors
		4 = draw intra subblocks. 0 = default
	      [min = 0, max = 7]
	-inp= : input file. NOTE: if not specified, defaults to stdin
	-out= : output file. NOTE: if not specified, defaults to stdout
	-y : do not prompt for confirmation when potentially overwriting an existing file
	-l<n> : set logging level to n (0 = none, 1 = error, 2 = warning, 3 = info, 4 = debug/all)
	-v : set verbose
```

------
NOTE: if -inp= and -out= are not specified, it will default to standard in / out (stdin/stdout).  
Only .yuv (one file containing all the frames) and .y4m files are supported as inputs to the encoder.

------

## Notes

This codec is by no means fully optimized, so there is a lot of room for performance gains. It performs quite well for what it is though.

------
If you have any questions feel free to leave a comment on YouTube OR
join the King's Crook Discord server :)

YouTube: https://www.youtube.com/@EMMIR_KC/videos

Discord: https://discord.gg/hdYctSmyQJ

itch.io: https://kingscrook.itch.io/kings-crook

------
## Example videos:

All videos are encoded at 30fps with a GOP length of 12.  
The H.264 file sizes were within a few kilobytes of their respective DSV2 file size.  
H.264 examples were encoded using https://github.com/lieff/minih264 using -speed0 (best quality)  
DSV2 examples were encoded with -effort=10 (best quality)  

minih264:


https://github.com/user-attachments/assets/4fa90a3c-38ad-45f3-b41b-3017ea20da6f

DSV2:


https://github.com/user-attachments/assets/9e939a4c-136f-4ee0-b54c-7a270ccf1aeb

------

minih264:


https://github.com/user-attachments/assets/f3930a65-db02-47d7-a2ac-f2c73c5bae80

DSV2:


https://github.com/user-attachments/assets/f9e07286-a8f3-4925-99c0-8754257bc6d5

