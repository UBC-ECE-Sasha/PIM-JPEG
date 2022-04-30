#!/bin/bash

# J.Nider 2022-04-29
# Measure CPU time to convert JPEGs to BMP using imagemagick
# assumes 'convert' is already installed and in the path

logfile=cpu-convert.log
path=data/imagenet/*.JPEG

echo $(date --iso-8601) > $logfile

for f in $path; do
	# the output looks weird because the output from 'time' starts with a blank line
	# start with a separator to avoid confusion
	echo "---" >> $logfile
	echo $f >> $logfile
	{ time convert $f $f.bmp; } 2>> $logfile
done

echo "Output in $logfile"
