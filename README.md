# PIM-JPEG
JPEG autoscaling in memory

This program currently performs the following using CPU only:
1. Read JPEG file headers
2. Decode huffman coded bitstream
3. Dequantization
4. Inverse Discrete Cosine Transform (DCT)
5. Convert YCbCr to RGB
6. Write RGB values to bitmap file

The implementation is heavily inspired by the following resources:
* CCITT specs: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
* Everything You Need to Know About JPEG Youtube series: https://www.youtube.com/watch?v=CPT4FSkFUgs&list=PLpsTn9TA_Q8VMDyOPrDKmSJYt1DLgDZU4
