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

### Note PDFs are out of date. They reflect the codec as it was when it was first uploaded here.

## DSV2 Features (refer to PDF in repo for more detail)

- compression using multiresolution subband analysis instead of DCT
   - also known as a wavelet transform
- up to quarter-pixel motion compensation
- 4:1:0, 4:1:1, 4:2:0, 4:2:2 (+ UYVY) and 4:4:4 chroma subsampling formats
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

### Zig build system

If you have Zig installed, you can use the build.zig file to compile the project. Building requires Zig version ≥`0.13.0`.

0. Ensure you have Zig & git installed

1. Clone this repo & enter the cloned directory:

```bash
git clone https://github.com/LMP88959/Digital-Subband-Video-2
cd Digital-Subband-Video-2
```

2. Build the binary with Zig:

```bash
zig build
```
> Note: If you'd like to specify a different build target from your host OS/architecture, simply supply the target flag. Example: `zig build -Dtarget=x86_64-linux-gnu`

3. Find the build binary in `zig-out/bin`. You can install it like so:

```bash
sudo cp zig-out/bin/dsv2 /usr/local/bin
```

Now, you should be all set to use the compiled `dsv2` binary.

## Running Encoder

Sample output:
```
Envel Graphics DSV v2.7 codec by EMMIR 2024-2025. encoder v10. decoder v2.
usage: ./dsv2 e [options]
sample usage: ./dsv2 e -inp=video.yuv -out=compressed.dsv -w=352 -h=288 -fps_num=24 -fps_den=1 -qp=85 -gop=15
------------------------------------------------------------
	-qp : quality percent. If -1 and ABR mode, it will auto-estimate a good starting qp for desired bitrate. If -1 and CRF mode, default to 85. -1 = default
	      [min = -1, max = 100]
	extra info: if ABR mode, the qp specified here will be the starting qp which will influence the quality of the beginning of your encoded video

	-effort : encoder effort. 0 = least effort, 10 = most effort. higher value -> better video, slower encoding. default = 10
	      [min = 0, max = 10]
	extra info: does not change decoding speed

	-w : width of input video. 352 = default
	      [min = 16, max = 16777216]
	extra info: must be divisible by two

	-h : height of input video. 288 = default
	      [min = 16, max = 16777216]
	extra info: must be divisible by two

	-gop : Group Of Pictures length. 0 = intra frames only, -1 = set to framerate (e.g 30fps source -> 30 GOP length), -1 = default
	      [min = -1, max = 2147483647]
	extra info: a good value is generally between 0.5 seconds and 10 seconds. e.g at 24 fps, GOP length of 12 is 0.5 seconds

	-fmt : chroma subsampling format of input video. 0 = 4:4:4, 1 = 4:2:2, 2 = 4:2:0, 3 = 4:1:1, 4 = 4:1:0, 5 = 4:2:2 UYVY, 2 = default
	      [min = 0, max = 5]
	extra info: 4:1:0 is one chroma sample per 4x4 luma block

	-nfr : number of frames to compress. -1 means as many as possible. -1 = default
	      [min = -1, max = 2147483647]
	extra info: unlike -sfr, this parameter works when piping from stdin

	-sfr : frame number to start compressing at. 0 = default
	      [min = 0, max = 2147483647]
	extra info: does not work when piping from stdin

	-noeos : do not write EOS packet at the end of the compressed stream. 0 = default
	      [min = 0, max = 1]
	extra info: useful for multithreaded encoding via concatenation

	-fps_num : fps numerator of input video. 30 = default
	      [min = 1, max = 16777216]
	extra info: used for rate control in ABR mode, otherwise it's just metadata for playback

	-fps_den : fps denominator of input video. 1 = default
	      [min = 1, max = 16777216]
	extra info: used for rate control in ABR mode, otherwise it's just metadata for playback

	-aspect_num : aspect ratio numerator of input video. 1 = default
	      [min = 1, max = 16777216]
	extra info: only used as metadata for playback

	-aspect_den : aspect ratio denominator of input video. 1 = default
	      [min = 1, max = 16777216]
	extra info: only used as metadata for playback

	-ipct : percentage threshold of intra blocks in an inter frame after which it is simply made into an intra frame. 90 = default
	      [min = 0, max = 100]
	extra info: can be used as a sort of scene change detection alternative if SCD is disabled

	-pyrlevels : number of pyramid levels to use in hierarchical motion estimation. 0 means auto-determine. 0 = default
	      [min = 0, max = 5]
	extra info: less than 3 levels gives noticeably bad results

	-rc_mode : rate control mode. 0 = constant rate factor (CRF), 1 = single pass average bitrate (ABR), 2 = constant quantization parameter (CQP). 0 = default
	      [min = 0, max = 2]
	extra info: ABR is recommended for hitting a target file size

	-rc_pergop : for non-CQP rate control. 0 = quality is updated per frame, 1 = quality is updated per GOP. 0 = default
	      [min = 0, max = 1]
	extra info: per GOP can be better for visual consistency

	-kbps : ONLY FOR ABR RATE CONTROL: bitrate in kilobits per second. 0 = auto-estimate needed bitrate for desired qp. 0 = default
	      [min = 0, max = 2147483647]
	extra info: adheres to specified frame rate

	-minqstep : min quality step when decreasing quality for CRF/ABR rate control, any step smaller in magnitude than minqstep will be set to zero, absolute quant amount in range [1, 400]. 2 = default (0.5%)
	      [min = 1, max = 400]
	extra info: generally not necessary to modify

	-maxqstep : max quality step for CRF/ABR rate control, absolute quant amount in range [1, 400]. 1 = default (0.25%)
	      [min = 1, max = 400]
	extra info: generally not necessary to modify

	-minqp : minimum quality. -1 = auto, -1 = default
	      [min = -1, max = 100]
	extra info: use it to limit the CRF/ABR rate control algorithm

	-maxqp : maximum quality. -1 = auto, -1 = default
	      [min = -1, max = 100]
	extra info: use it to limit the CRF/ABR rate control algorithm

	-iminqp : minimum quality for intra frames. -1 = auto, -1 = default
	      [min = -1, max = 100]
	extra info: use it to limit the CRF/ABR rate control algorithm

	-stabref : period (in # of frames) to refresh the stability block tracking. 0 = auto-determine. 0 = default
	      [min = 0, max = 2147483647]
	extra info: recommended to keep as auto-determine but good values are typically between half the framerate and twice the framerate

	-scd : do scene change detection. 1 = default
	      [min = 0, max = 1]
	extra info: let the encoder insert intra frames when it decides that the scene has changed (sufficient difference between consecutive frames)

	-tempaq : do temporal adaptive quantization. If disabled, spatial methods will be used instead. 1 = default
	      [min = 0, max = 1]
	extra info: recommended to keep enabled, increases quality on features of the video that stay still

	-bszx : override block sizes in the x (horizontal) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32
	      [min = -1, max = 1]
	extra info: 16 is recommended for < 1920x1080 content

	-bszy : override block sizes in the y (vertical) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32
	      [min = -1, max = 1]
	extra info: 16 is recommended for < 1920x1080 content

	-scpct : scene change percentage. 55 = default
	      [min = 0, max = 100]
	extra info: decrease to make scene changes more common, increase to make them more infrequent

	-skipthresh : skip block threshold. -1 = disable. 0 = default, larger value means more likely to mark a block as skipped.
	      [min = -1, max = 2147483647]
	extra info: generally not necessary to modify

	-varint : intra frames that are created outside of the normal GOP cycle reset the GOP cycle if 1. 1 = default
	      [min = 0, max = 1]
	extra info: generally good to keep this enabled unless you absolutely need an intra frame to exist every 'GOP' frames

	-psy : enable/disable psychovisual optimizations. 255 = default
	      [min = 0, max = 255]
	extra info: can hurt or help depending on content. can be beneficial to try both and see which is better.
		currently defined bits (bit OR together to get multiple at the same time):
		1 = adaptive quantization
		2 = content analysis
		4 = I-frame visual masking
		8 = P-frame visual masking
		16 = adaptive ringing transform


	-dib : enable/disable boosting the quality of dark intra frames. 1 = default
	      [min = 0, max = 1]
	extra info: helps retain details in darker scenes

	-y4m : set to 1 if input is in YUV4MPEG2 (Y4M) format, 0 if raw YUV. 0 = default
	      [min = 0, max = 1]
	extra info: not all metadata will be passed through, Y4M parser is not a complete parser and some inputs could result in error

	-ifilter : enable/disable intra frame deringing filter (essentially free assuming reasonable GOP length). 1 = default
	      [min = 0, max = 1]
	extra info: helps reduce ringing introduced at lower bit rates due to longer subband filters

	-pfilter : enable/disable inter frame cleanup filter (small decoding perf hit but very noticeable increase in quality). 1 = default
	      [min = 0, max = 1]
	extra info: beneficial to coding efficiency and visual quality, highly recommended to keep enabled UNLESS source is very noisy

	-psharp : inter frame sharpening. 0 = disabled, 1 = enabled, 1 = default
	      [min = 0, max = 1]
	extra info: smart image sharpening, helps reduce blurring in motion

	-inp= : input file. NOTE: if not specified, defaults to stdin
	-out= : output file. NOTE: if not specified, defaults to stdout
	-y : do not prompt for confirmation when potentially overwriting an existing file
	-l<n> : set logging level to n (0 = none, 1 = error, 2 = warning, 3 = info, 4 = debug/all)
	-v : set verbose
```

## Running Decoder

Sample output:
```
Envel Graphics DSV v2.7 codec by EMMIR 2024-2025. encoder v10. decoder v2.
usage: ./dsv2 d [options]
sample usage: ./dsv2 d -inp=video.dsv -out=decompressed.yuv -out420p=1
------------------------------------------------------------
	-out420p : convert video to 4:2:0 chroma subsampling before saving output. 0 = default
	      [min = 0, max = 1]
	-y4m : write output as a YUV4MPEG2 (Y4M) file. 0 = default
	      [min = 0, max = 1]
	-postsharp : postprocessing/decoder side frame sharpening. 0 = disabled, 1 = enabled, 0 = default
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
