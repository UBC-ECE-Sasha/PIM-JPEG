#!/bin/python3

# 2022-03-01 J.Nider
# Plot the scalability experiment - a line graph of 'files per second' vs
# 'number of files'. The number of DPUs increases with the number of files.

# expects a CSV file in the form:
# files,data,rank_min,rank_max,total

from matplotlib.colors import ListedColormap
import pandas as pd
import matplotlib.pyplot as plt
import argparse

def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description="Create plot for 'scalability'")
	parser.add_argument("--csvfile", help="Input data file in csv format", required=True, type=str)
	parser.add_argument("--output", help="Output file name", default="output.png", type=str)
	return parser.parse_args()

def draw():
	cmap = ListedColormap(['#f00000', '#008080', '#1020a0', '#00a0ff', '#a0ffff'])
	plt.rcParams['figure.figsize'] = (7, 4)
	ax = df.plot(x='files', y='fps', colormap=cmap)
	ax.semilogx()
	ax.set_xticks(ticks=df['files'])
	ax.set_xticklabels(labels=df['files'])
	ax.tick_params(labelrotation=41.6)
	ax.set_xlabel('Number of Files Processed')
	ax.set_ylabel('Files per second')
	ax.set_title('Scalability of Decoding Throughput')
	ax.legend().remove()

	plt.savefig(config.output, bbox_inches='tight', pad_inches=0.10, dpi=300)

# read the command line arguments
config = parse_args()

# load the data file
df = pd.read_csv(config.csvfile)

# add column
df['fps'] = df['files'] / df['total']
print(df)

# draw the graph
draw()
plt.close()
