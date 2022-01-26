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
    args = commandArgs()
    logging.basicConfig(stream=sys.stderr, level=args.verbosity)

    resultsFile = open(args.outputResults, 'w')
    resultsWriter = csv.writer(resultsFile)
    csvHeader = ['image', 'corrupted']
    if not args.PSNRonly:
        csvHeader.append('SSIM')
        if args.channelResults:
            csvHeader += ['SSIM-R', 'SSIM-G', 'SSIM-B']
    csvHeader.append('PSNR')
    if args.channelResults:
        csvHeader += ['PSNR-R', 'PSNR-G', 'PSNR-B']
    csvHeader += ['reference path', 'output path']
    resultsWriter.writerow(csvHeader)

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
                try:
                    results = compare_img(refPath, outPath, PSNRonly=args.PSNRonly)
                except Exception as e:
                    logging.warning(str(e))
                    continue
                corrupted = results["PSNR"] < args.PSNRthreshold
                if not args.PSNRonly:
                    corrupted = corrupted or results["SSIM"] < args.SSIMthreshold
                if corrupted:
                    logging.info("Output image %s may be corrupted.", name)

                row = [refName, corrupted]
                if not args.PSNRonly:
                    row.append(results["SSIM"])
                    if args.channelResults:
                        for channel in ["SSIM-R", "SSIM-G", "SSIM-B"]:
                            try:
                                row.append(channel)
                            except Exception:
                                row.append(" ")
                row.append(results["PSNR"])
                if args.channelResults:
                    for channel in ["PSNR-R", "PSNR-G", "PSNR-B"]:
                        try:
                            row.append(channel)
                        except Exception:
                            row.append(" ")
                row += [refPath, outPath]
                resultsWriter.writerow(row)

            else:
                logging.warning("Couldn't find reference image for: %s", outPath)

def compare_img(refPath, outPath, PSNRonly=False):
    name = outPath
    refName = refPath
    outImg = Image.open(outPath)
    refImg = Image.open(refPath)
    results = {}

    # check dimensions
    if outImg.size != refImg.size:
        logging.debug("Output image size: %s", str(outImg.size))
        logging.debug("Reference image size: %s", str(refImg.size))
        raise Exception("Image size mismatch for image " + name)

    if outImg.mode != refImg.mode:
        logging.info("Image mode mismatch for image " + name + ". Output converted to reference format.")
        logging.debug("Output image mode: %s", outImg.mode)
        logging.debug("Reference image mode: %s", refImg.mode)
        outImg = outImg.convert(refImg.mode)

    try:
        refPixels = np.asarray(refImg)
    except Exception as e:
        raise Exception("Reference image %s corrupted with error: %s".format(refName, e))
    try:
        outPixels = np.asarray(outImg)
    except Exception as e:
        raise Exception("Output image %s corrupted with error: %s".format(name, e))

    if outPixels.shape != refPixels.shape:
        logging.debug("Output image dimensions: %s", str(outPixels.shape))
        logging.debug("Reference image dimensions: %s", str(refPixels.shape))
        logging.debug("Output image mode: %s", outImg.mode)
        logging.debug("Reference image mode: %s", refImg.mode)
        raise Exception("Image dimension mismatch for image %s".format(name))

    results["mode"] = refImg.mode

    if len(refImg.mode) == 1: # single channel images are 2D instead of 3D arrays
        results["PSNR"] = metrics.peak_signal_noise_ratio(refPixels, outPixels)
        if not PSNRonly:
            results["SSIM"] = metrics.structural_similarity(refPixels, outPixels)
    else:
        psnrSum = 0
        ssimSum = 0
        for i, channel in enumerate(list(refImg.mode)):
            psnr = metrics.peak_signal_noise_ratio(refPixels[:, :, i], outPixels[:, :, i])
            results["PSNR-" + channel] = psnr
            psnrSum += psnr
            if not PSNRonly:
                ssim = metrics.structural_similarity(refPixels[:, :, i], outPixels[:, :, i])
                results["SSIM-" + channel] = ssim
                ssimSum += ssim

        results["PSNR"] = psnrSum/len(refImg.mode)
        if not PSNRonly:
            results["SSIM"] = ssimSum/len(refImg.mode)

    return results

    


def commandArgs():
    parser = ArgumentParser(description="Collect image quality metrics.")
    parser.add_argument("--imgDir", "-i", help="directory in which to find images",
        default="../data")
    parser.add_argument("--refFmt", "-r", help="regex for filename extension/format of reference images",
        default="(?i)(.jpg|.jpeg|.png)")
    parser.add_argument("--outFmt", "-t", help="filename extension/format of output images",
        default=".bmp")
    parser.add_argument("--outputResults", "-o", help="name of csv file to output results to",
        default="qual.csv")
    parser.add_argument("--channelResults", "-c", help="whether or not to output PSNR and SSIM for each channel or just overall",
        action="store_true")
    parser.add_argument("--PSNRonly", "-n", help="only calculate PSNR",
        action="store_true")
    parser.add_argument("--PSNRthreshold", "-p", help="PSNR threshold to consider a file incorrect",
        type=float, default=30.0)
    parser.add_argument("--SSIMthreshold", "-s", help="SSIM threshold to consider a file incorrect",
        type=float, default=0.95)
    parser.add_argument("--verbosity", "-v", help="increase output verbosity",
        type=int, default=logging.INFO)

    return parser.parse_args()
    
if __name__ == '__main__':
    main()