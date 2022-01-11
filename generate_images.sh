#!/bin/bash

for f in {1..1024}
do
    cp images/img1.jpg  images/img$f.jpg
    cp images2/img1.jpg  images2/img$f.jpg
done
