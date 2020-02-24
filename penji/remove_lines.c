/*
 * remove_lines.c
 *
 * A standalone program that reduce the number of lines in the jpeg file.
 * This essentially has the effect of cropping the bottom of the image.
 * By default we crop half of the image. We might be able to further
 * crop a parametrized numbe of lines.
 *
 * The key is to generate a valid jpeg image without decompressing the entire
 * image.
 *
 * Usage:
 * 	remove_lines [-n num_lines] <inputfile> <outputfile>
 */
