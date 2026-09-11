# Digital-Subband-Video-2
------

<h2 align="center">As of August 29, 2026 the DSV2 bitstream is frozen (v2.8)</h1>
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



https://github.com/user-attachments/assets/e5695fa1-afa7-4205-9901-1993569efd88






DSV2:





https://github.com/user-attachments/assets/37283f25-4ca7-4587-aee6-5b619301b8d0







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
- new HVS based intra frame adaptive quantization
- more complex scene change detection
- hierarchical motion estimation
- better temporal adaptive quantization
- written to be compatible with C89

Approximate quality guide for encoding:  
 qp 0-20 = very low quality    
 qp 20-30 = low quality  
 qp 30-40 = medium-low quality  
 qp 40-50 = medium quality  
 qp 50-70 = medium-high quality  
 qp 70-90 = high quality, around qp 70 is the beginning of 'visually lossless'  
 qp 90-99 = very high quality (visually lossless)  
 qp 100 = mathematically lossless  

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

## The BC2 Color Space

A pseudo-perceptual color space designed to be faster than existing perceptual color spaces while also demonstrating more appealing subsampling and quantization artifacts compared to typical YCbCr.  
It can easily be used as a drop-in replacement for 8-bit YUV, the image/video viewer just needs to know how to decode BC2, see the `bc2` directory in the repository to learn more!  

Original vs YCbCr 4:2:0 @ 132kb vs BC2 4:2:0 @ 130kb **(click to expand/download)**:  

<img width="256" height="256" alt="crazy" src="https://github.com/user-attachments/assets/659a1bbb-d605-4935-97c7-9a636da4d2cb" />
<img width="256" height="256" alt="test_dsv2_latest_132kb" src="https://github.com/user-attachments/assets/c8414413-9054-47d6-abd6-0609fac015a4" />
<img width="256" height="256" alt="test_dsv2_bc2_130kb" src="https://github.com/user-attachments/assets/aabe8072-9e10-438a-b83c-72b44f8c557c" />


------

## Example videos:

The H.264 file sizes were within a few kilobytes of their respective DSV2 file size.  
H.264 examples were encoded using https://github.com/lieff/minih264 using -speed0 (best quality) *unless otherwise stated*  
DSV2 examples were encoded with -effort=10 (best quality)

------

Husky CIF - 29.97fps, GOP = 60
minih264:




https://github.com/user-attachments/assets/4ae22745-0d74-4a37-96b8-dc427dce720b




DSV2:




https://github.com/user-attachments/assets/fb0b414c-4ff1-4ab6-a823-e205f358f09e





------

Mobile CIF - 29.97fps, GOP = 60
minih264:



https://github.com/user-attachments/assets/5562d96d-092c-46da-b0ab-1063bd0587a6



DSV2:





https://github.com/user-attachments/assets/0f66f580-60f9-4d54-b19a-c8dc9a9cbf1d





------

Parkrun 1280x720 - 50fps, GOP = 250

minih264:


https://github.com/user-attachments/assets/bcc408bd-6d5b-4725-b26e-307eee21ba56

x264 (encoded via ffmpeg with `-c:v libx264 -preset superfast -crf 29.8 -refs 1 -coder vlc -bf 0 -g 250`):


https://github.com/user-attachments/assets/a59c4027-4d6e-4b82-929b-4e344ca931d8

DSV2:
(Note this video here had to be re-encoded to fit under 10MB. I re-encoded with x264 CRF 22 with the veryslow preset)




https://github.com/user-attachments/assets/1d97509c-e643-4443-96f9-b50f4cc20e7c


