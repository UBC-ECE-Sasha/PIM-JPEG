#!/bin/bash

IMAGES_DIR="/mnt/nvme/images"
OUTPUT_DIR="/mnt/nvme/output_dir"

FULL_STRING=""

for i in {1..600}
do
    FULL_STRING="$FULL_STRING $IMAGES_DIR/img$i.jpg "
done
    ./host-8 $FULL_STRING

