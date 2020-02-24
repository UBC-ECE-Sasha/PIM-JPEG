#!/usr/bin/bash

# compile remove_lines
#gcc remove_lines.c -o remove_lines -w -Ilibjpeg-turbo/build/ -Llibjpeg-turbo/build/ -ljpeg
gcc remove_lines.c -o remove_lines -w -Ilibjpeg-turbo/ -Llibjpeg-turbo/build/ -Wl,-rpath=./libjpeg-turbo/build/ -ljpeg
