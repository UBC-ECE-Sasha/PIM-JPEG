#!/bin/bash

IMAGES_DIR="./images"
#IMAGES_DIR="./images2"

NUM_DPUS=$1
NUM_RANKS=$2
FULL_STRING=""

for i in $(seq 1 $NUM_DPUS)
do
    FULL_STRING="$FULL_STRING $IMAGES_DIR/img$i.jpg "
done

FULL_STRING="-d -n $NUM_DPUS -k $NUM_RANKS $FULL_STRING"
echo $FULL_STRING

./host-8  $FULL_STRING

