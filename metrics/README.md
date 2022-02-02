# Installing

```
pip3 install -r metrics/requirements.txt
```

# Measuring image quality

The image quality metrics compares compressed image files with their decompressed output using [peak-signal-to-noise ratio (PSNR)](https://en.wikipedia.org/wiki/Peak_signal-to-noise_ratio) and [structural similarity index (SSIM)](https://en.wikipedia.org/wiki/Structural_similarity).

## Examples

The following is an example where the script uses .JPEG reference images on images in the imagenet directory.

```
python3 img_quality.py -r .JPEG -i ../data/imagenet
```
