#!/bin/python3

# 2022-05-02 J.Nider
# Plot the detail timing breakdown - a stacked histogram showing percent of total
# time for each stage in the JPEG decoding process.

# expects a CSV file in the form:
# stage,time

from matplotlib.colors import ListedColormap
import pandas as pd
import matplotlib.pyplot as plt
import argparse
import matplotlib.ticker as mtick
from matplotlib.ticker import PercentFormatter

def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description="Create plot for 'detailed timing'")
	parser.add_argument("--csvfile", help="Input data file in csv format", required=True, type=str)
	parser.add_argument("--output", help="Output file name", default="timing.png", type=str)
	return parser.parse_args()

def draw():
	ax = df.plot(kind='barh', stacked=True, align='edge',
        title='Breakdown of DPU Decoding Time', 
        figsize=(10,4))

	# remove whitespace
	ax.margins(x=0)

	# set X-axis to print percentage
	ax.xaxis.set_major_formatter(PercentFormatter(xmax=1))

	# remove Y-axis label
	ax.yaxis.set_visible(False)

	# Matplotlib idiom to reverse legend entries 
	handles, labels = ax.get_legend_handles_labels()
	ax.legend(reversed(handles), reversed(labels))
	#ax.legend(loc='upper center', shadow=True, fontsize='x-large', ncol=2)
	ax.legend(loc='upper center', shadow=True, ncol=2)

	# ax.bar_label() can only be used from matplotlib 3.3.4 (I'm using 3.1.2 now)
	for p in ax.patches:
		value = p.get_width()
		if value > 0.10:
			ax.annotate(f'{value*100:0.1f}%', (p.get_x() + p.get_width() / 2., 0), ha = 'center', va = 'center', xytext = (0, 10), textcoords = 'offset points')

	plt.savefig(config.output, bbox_inches='tight', pad_inches=0.10, dpi=300)

# read the command line arguments
config = parse_args()

# load the data file
df = pd.read_csv(config.csvfile)

# convert to percentages
df = df.div(df.sum(axis=1), axis=0)

print(df)

# draw the graph
draw()
plt.close()
