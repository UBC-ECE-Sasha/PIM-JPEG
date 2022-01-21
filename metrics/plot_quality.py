"""
plot_quality.py

Plot results from img_quality.py

author: Jackson Dagger
email: JacksonDDagger at gmail
date: 2022-01-20
"""

from argparse import ArgumentParser
import numpy as np
import pandas as pd
import os, sys, logging, re
import matplotlib.pyplot as plt

def main():
    args = commandArgs()
    logging.basicConfig(stream=sys.stderr, level=args.verbosity)
    filename = args.filename[0]

    if not re.match(r"^.*\.(csv)$", filename):
        logging.error("Please pass a csv.")
        return

    df = pd.read_csv(filename)
    hist = df.hist(column=["SSIM", "PSNR"])

def commandArgs():
    parser = ArgumentParser(description="Plot image quality metrics csv.")
    parser.add_argument("filename", nargs=1, help="csv file with quality results")
    parser.add_argument("--verbosity", "-v", help="increase output verbosity",
        type=int, default=logging.INFO)

    return parser.parse_args()

if __name__ == '__main__':
    main()