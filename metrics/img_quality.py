"""
img_quality.py

This script can be used to measure Peak Signal to Noise Ratio (PSNR) and
Structural Similartity Index (SSIM) between jpgs and decoded images and
highlight potential errors.

author: Jackson Dagger
email: JacksonDDagger at gmail
date: 2022-01-04
"""

from argparse import ArgumentParser
from skimage import metrics
from PIL import Image, ImageFile
import numpy as np
import os, sys, logging, csv, re
from statistics import mean

ImageFile.LOAD_TRUNCATED_IMAGES = True

def main():
    parser = ArgumentParser(description="Collect image quality metrics.")
    parser.add_argument("--imgDir", "-i", help="directory in which to find images",
        default="../data")
    parser.add_argument("--refFmt", "-r", help="regex for filename extension/format of reference images",
        default="(?i)(.jpg|.jpeg|.png)")
    parser.add_argument("--outFmt", "-t", help="filename extension/format of output images",
        default=".bmp")
    parser.add_argument("--outputResults", "-o", help="filename extension/format of output images",
        default="img_quality_results.csv")
    parser.add_argument("--PSNRonly", "-n", help="only calculate PSNR",
        action="store_true")
    parser.add_argument("--PSNRthreshold", "-p", help="PSNR threshold to consider a file incorrect",
        type=float, default=30.0)
    parser.add_argument("--SSIMthreshold", "-s", help="SSIM threshold to consider a file incorrect",
        type=float, default=0.95)
    parser.add_argument("--verbosity", "-v", help="increase output verbosity",
        type=int, default=logging.INFO)

    args = parser.parse_args()

    logging.basicConfig(stream=sys.stderr, level=args.verbosity)

    resultsFile = open(args.outputResults, 'w')
    results = csv.writer(resultsFile)
    csvHeader = ['image']
    if not args.PSNRonly:
        csvHeader += ['SSIM-R', 'SSIM-G', 'SSIM-B', 'SSIM']
    csvHeader += ['PSNR-R', 'PSNR-G', 'PSNR-B', 'PSNR',
        'reference path', 'output path']
    results.writerow(csvHeader)

    for path, subdirs, files in os.walk(args.imgDir):
        for name in list(filter(lambda x: x[-len(args.outFmt):] == args.outFmt, files)):        
            outPath = os.path.join(path, name)
            # find reference image
            imgName = name[:-(len(args.outFmt)+4)]

            ldir = os.listdir(path)
            imgMatches = list(filter(lambda x:x.startswith(imgName), ldir))
            refPath = None
            pattern = re.compile(args.refFmt)

            for f in imgMatches:
                if pattern.search(f) != None:
                    refName = f
                    refPath = os.path.join(path, f)
                    break

            if refPath != None and os.path.isfile(refPath):
                outImg = Image.open(outPath)
                refImg = Image.open(refPath)

                # check dimensions
                if outImg.size != refImg.size:
                    logging.warning("Image size mismatch for image %s", name)
                    logging.debug("Output image size: %s", str(outImg.size))
                    logging.debug("Reference image size: %s", str(refImg.size))
                    continue

                if outImg.mode != refImg.mode:
                    logging.info("Image mode mismatch for image %s. Output converted to reference format.", name)
                    logging.debug("Output image mode: %s", outImg.mode)
                    logging.debug("Reference image mode: %s", refImg.mode)
                    outImg = outImg.convert(refImg.mode)

                try:
                    refPixels = np.asarray(refImg)
                except Exception as e:
                    logging.warning("Reference image %s corrupted with error: %s", refName, e)
                    continue
                try:
                    outPixels = np.asarray(outImg)
                except Exception as e:
                    logging.warning("Output image %s corrupted with error: %s", name, e)
                    continue

                if outPixels.shape != refPixels.shape:
                    logging.warning("Image dimension mismatch for image %s", name)
                    logging.debug("Output image dimensions: %s", str(outPixels.shape))
                    logging.debug("Reference image dimensions: %s", str(refPixels.shape))
                    logging.debug("Output image mode: %s", outImg.mode)
                    logging.debug("Reference image mode: %s", refImg.mode)
                    continue

                if refImg.mode == "L":
                    psnrl = metrics.peak_signal_noise_ratio(refPixels, outPixels)
                    psnr = [psnrl]*4
                    if not args.PSNRonly:
                        ssiml = metrics.structural_similarity(refPixels, outPixels)
                        ssim = [ssiml]*4

                elif refImg.mode == "RGB":
                    psnr = [0.0]*4
                    for i in range(3):
                        psnr[i] = metrics.peak_signal_noise_ratio(refPixels[:, :, i], outPixels[:, :, i])
                    psnr[3] = mean(psnr[:3])

                    if not args.PSNRonly:
                        ssim = [0.0]*4
                        for i in range(3):
                            ssim[i] = metrics.structural_similarity(refPixels[:, :, i], outPixels[:, :, i])
                        ssim[3] = mean(ssim[:3])
                else:
                    logging.warning("Unsupported image mode for image %s", name)
                    logging.debug("Image mode: %s", outImg.mode)
                    continue

                row = [refName]
                if not args.PSNRonly:
                    row += ssim
                row += psnr + [refPath, outPath]
                results.writerow(row)

                corrupted = psnr[3] < args.PSNRthreshold
                if not args.PSNRonly:
                    corrupted = corrupted or ssim[3] < args.SSIMthreshold
                if corrupted:
                    logging.info("Output image %s may be corrupted.", name)
            else:
                logging.warning("Couldn't find reference image for: %s", outPath)
    

if __name__ == '__main__':
    main()