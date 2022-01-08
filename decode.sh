#!/bin/bash

IMAGES_DIR="./images2"

NUM_DPUS=$1
FULL_STRING=""

for i in $(seq 1 $NUM_DPUS)
do
    FULL_STRING="$FULL_STRING $IMAGES_DIR/img$i.jpg "
done

echo $FULL_STRING

./host-8 $FULL_STRING

