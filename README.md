# Digital-Subband-Video-2
------

<h2 align="center">As of June 20, 2025 the DSV2 bitstream is frozen (v2.8)</h1>
<p align="center">
<img width="300" height="300" src="https://github.com/user-attachments/assets/ec4022d6-c60a-408f-979e-5845086f553a">
</p>

DSV2 is a lossy/lossless video codec using wavelets and block-based motion compensation.
It performs best at medium-low to medium-high bitrates at resolutions between ~352x288 (CIF) to 1920x1080 (FHD).
Comparable to MPEG-4 Part 2 and Part 10 (H.264) (P frames only) in terms of efficiency and quality.

------
## Example comparison (More can be found at the bottom of the README):

Stefan CIF
minih264:



https://github.com/user-attachments/assets/a309edb4-3866-4220-9c6b-61ced3acbbcd




DSV2:




https://github.com/user-attachments/assets/f6cbf927-3e2a-4078-a086-e5fb47ea66ab






------

### Note PDFs are out of date. They reflect the codec as it was when it was first uploaded here.

## DSV2 Features (refer to PDF in repo for more detail)

- compression using multiresolution subband analysis instead of DCT
   - also known as a wavelet transform
- up to quarter-pixel motion compensation
- 4:1:0, 4:1:1, 4:2:0, 4:2:2 (+ UYVY) and 4:4:4 chroma subsampling formats
- adaptive quantization
- in-loop filtering
- lossless coding support
- intra and inter frames with variable length closed GOP
   - no bidirectional prediction (also known as B-frames). Only forward prediction with previous picture as reference

## Improvements and New Features since DSV1

- in-loop filtering after motion compensation
- more adaptive quantization potential
- skip blocks for temporal stability
- new subband filters + support for adaptive subband filtering
- better motion compensation through Expanded Prediction Range Mode (EPRM)
- quarter pixel compensation
- lossless coding support
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

Sample usage:
```
./dsv2 e -inp=video.y4m -out=compressed.dsv -y4m=1 -qp=60 -gop=48
```

## Running Decoder

Sample usage:
```
./dsv2 d -inp=video.dsv -out=decompressed.y4m -y4m=1 -out420p=1
```

------
NOTE: if -inp= and -out= are not specified, it will default to standard in / out (stdin/stdout).
Only .yuv (one file containing all the frames) and .y4m files are supported as inputs to the encoder.

------

## Notes

This codec is by no means fully optimized, so there is a lot of room for performance gains. It performs quite well for what it is though.

## Wavelet Codec Testbench

Check out this open source / work in progress wavelet codec testbench by Gianni Rosato!  
https://github.com/gianni-rosato/wavelet-bench

------
If you have any questions feel free to leave a comment on YouTube OR
join the King's Crook Discord server :)

YouTube: https://www.youtube.com/@EMMIR_KC/videos

Discord: https://discord.gg/hdYctSmyQJ

itch.io: https://kingscrook.itch.io/kings-crook

------
## Example videos:

All videos are encoded at 29.97fps with a GOP length of 12.
The H.264 file sizes were within a few kilobytes of their respective DSV2 file size.
H.264 examples were encoded using https://github.com/lieff/minih264 using -speed0 (best quality)
DSV2 examples were encoded with -effort=10 (best quality)
------

Husky CIF
minih264:


https://github.com/user-attachments/assets/2ddb2d57-fe48-4cef-b755-42230393c6e9


DSV2:



https://github.com/user-attachments/assets/4a93cc8e-0618-4993-9efb-f2a121a75aed



------

Mobile CIF
minih264:


https://github.com/user-attachments/assets/4fa90a3c-38ad-45f3-b41b-3017ea20da6f

DSV2:




https://github.com/user-attachments/assets/27bf1e6a-3668-4d5c-a66c-a9ea7c7c822d




------
Bus CIF

minih264:


https://github.com/user-attachments/assets/f3930a65-db02-47d7-a2ac-f2c73c5bae80

DSV2:


https://github.com/user-attachments/assets/86805c29-011a-4cb7-bc03-105616e183cf


