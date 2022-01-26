# Measuring image quality

Scripts to compare compressed image files with their decompressed output using [peak-signal-to-noise ratio (PSNR)](https://en.wikipedia.org/wiki/Peak_signal-to-noise_ratio) and [structural similarity index (SSIM)](https://en.wikipedia.org/wiki/Structural_similarity). 

## img_quality.py

The script searches the given directories for compressed images and their corresponding decompressed bitmaps to compare them. Currently, only JPG, JPEG and PNG compressed images in RGB are supported. The script decompresses the images with an implementation that is assumed to be correct and compares that with the result from the decompression method being tested.

### Command line arguments

**--imgDir -i** Directory in which to find reference and decompressed images.  
**--refFmt -r** Regex for filename extension/format of reference images.  
**--outFmt -t** Extension/format of output images.  
**--outputResults -o** Filename of output csv.  
**--PSNRonly -n** only calculate PSNR (significantly faster)  
**--PSNRthreshold -p** PSNR threshold to consider a file incorrect and mark as corrupted. Currently experimentally determined and will need to be lowered for extremely compressed images.  
**--SSIMthreshold -s** SSIM threshold to consider a file incorrect and mark as corrupted. Currently experimentally determined and will need to be lowered for extremely compressed images.  
**--verbosity -v** Set logging verbosity according to [python's logging module.](https://docs.python.org/3/library/logging.html#logging-levels)

### Examples

The following is an example where the script uses .JPEG reference images on images in the imagenet directory.

```
python3 img_quality.py -r .JPEG -i ../data/imagenet
```