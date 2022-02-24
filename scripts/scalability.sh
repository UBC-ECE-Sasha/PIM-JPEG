#!/bin/bash

# 2022-23-02 J.Nider

# Measure scalability across a given number of files (=DPUs)

prog=host-1
input_file=data/imagenet/n02116738_124.JPEG
max_files=65536
output=scalability.log

make clean
make STATS=1
if [[ ! -f $prog ]]; then
	echo Error compiling $prog
	exit -1
fi

echo $(date --iso-8601) > $output
for (( file_count=1; file_count < $max_files; file_count = $file_count * 2 )); do
	cmd="./host-1 -S -m $file_count $input_file"
	echo $cmd
	$cmd >> $output
done

echo "Log file in $output"
