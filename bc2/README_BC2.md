# The BC2 Transform

The source code will still be the best reference for a working implementation but this document might clear up some of the math.  
There is a lot of scale factor manipulation taking place in the actual implementation due to the integer-only requirement.  

---

## These are the main transformation matrices:  

Forward 3x3 (F):

[ 81, 139, 20 ] / (240 * 5)  
[ 51, 169, 20 ] / (240 * 5)  
[ 66, 54, 120 ] / (240 * 5)

Inverse 3x3 (I):

[ 32, -26, -1 ]  
[ -8, 14, -1 ]  
[ -14, 8, 11 ]  

---

## sRGB to BC2 consists of the following steps:  

Given 3 channel `(r,g,b)` input as 8-bit per channel sRGB color...

### 1. Convert sRGB to 'linear RGB'
`(lr,lg,lb) = square(r,g,b)` _# note the squaring has a rounding term, see the source code_  

### 2. Transform to preliminary BSI
`(pb,ps,pi) = (lr,lg,lb) * F`

### 3. Square root
_note the square root has some other scaling, see the source code_  
`(pb,ps,pi) = sqrt(pb,ps,pi)`

### 4. Haar-like transform
_note there are other scale factors here, refer to the source code for those_  
`b = pb + ps`  
`s = ps - pb`  
`i = pi - b`  _# note this is using b (computed above), not pb_  

### 5. Clipping and adjusting
apply clipping to 8-bit range and offset s/i channels by 128 so that they become unsigned values.  

---

## BC2 to sRGB consists of the following steps:  

Given 3 channel `(b,s,i)` input as 8-bit per channel BC2 color...

### 1. Undo adjustment
apply offset of -128 to s/i channels so that they become signed values again.  
_note there are other scale factors here, refer to the source code for those_  

### 2. Undo Haar-like transform
`(pb,ps,pi) = (b-s,b+s,b+i)`  

### 3. Square
`(pb,ps,pi) = (pb,ps,pi)^2`

### 4. Transform to preliminary sRGB
_note there are other scale factors here, refer to the source code for those_  
`(pr,pg,pb) = (pb,ps,pi) * I`


### 5. Convert to 'sRGB'
`(r,g,b) = revmap(pr,pg,pb)`

The revmap (reverse mapping) table applies the square root (to go back to ~sRGB) and scales it down to an 8-bit value.  
With the max expected value being (2560 * 4 - 1):  
`sqrt((2560*4-1) * pow(2, 17)) * 29309) / pow(2, 22) = ~255.99`  
where 29309 is just a scale factor that gets us the scaling we want.  

---