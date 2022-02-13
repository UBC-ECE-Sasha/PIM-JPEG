# bin/bash
# Script to copy reference JPEG and convert bmp to png for easy comparison
# 
# Usage: ./convert.sh image_class image_number
# ex. ./convert.sh n04008634 56

CAT=$1
IMG=$2

JPEG="../tiny-imagenet-200/train/${CAT}/images/${CAT}_${IMG}.JPEG"
BMP="../tiny-imagenet-200-dpu/train/${CAT}/images/${CAT}_${IMG}-dpu.bmp"
PNG="../tiny-imagenet-200-dpu/train/${CAT}/images/${CAT}_${IMG}-dpu.png"

cp $JPEG .
mogrify -format png $BMP
mv $PNG .