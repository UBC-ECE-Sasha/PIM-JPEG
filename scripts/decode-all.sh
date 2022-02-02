# bin/bash

TRAINDIR="../tiny-imagenet-200/train/"

for d in $TRAINDIR*; do
    ./host-1 -d $d/images/*
done