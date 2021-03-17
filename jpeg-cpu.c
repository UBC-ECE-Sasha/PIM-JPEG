#include <stdio.h>
#include <string.h>
#include <byteswap.h>
#include "jpeg-cpu.h"

#define MAX_COMP_INFO 3
#define DCTSIZE 8
#define MAX_COMPS_IN_SCAN 4
#define MAX_SAMP_FACTOR 4
#define MAX(_a, _b) (_a > _b ? _a : _b)

/* Markers: CCITT Rec T.81 page 32 */
enum markers {
	/* Start Of Frame non-differential, Huffman coding */
	M_SOF0 = 0xC0,
	M_SOF1,
	M_SOF2,
	M_SOF3,

	M_DHT = 0xC4, 								/* Define Huffman Tables */

	/* Start Of Frame differential, Huffman coding */
	M_SOF5 = 0xC5,
	M_SOF6,
	M_SOF7,

	/* Start Of Frame non-differential, arithmetic coding */
	M_JPG = 0xC8,
	M_SOF9,
	M_SOF10,
	M_SOF11,

	M_DAC = 0xCC,								/* Define Arithmetic Coding */

	/* Start Of Frame differential, arithmetic coding */
	M_SOF13,										/* Differential sequential DCT */
	M_SOF14,										/* Differential progressive DCT */
	M_SOF15,										/* Differential lossless */

	M_RST_FIRST = 0xD0,						/* Restart interval termination */
	M_RST_LAST = 0xD7,						/* Restart interval termination */

	M_SOI = 0xD8,								/* Start Of Image (beginning of datastream) */
	M_EOI,										/* End Of Image (end of datastream) */
	M_SOS,										/* Start Of Scan (begins compressed data) */
	M_DQT,										/* Define quantization table */
	M_DNL,
	M_DRI,
	M_DHP,
	M_EXP,
	M_APP_FIRST = 0xE0,						/* Application-specific marker, type N */
	M_APP_LAST = 0xEF,
	M_EXT_FIRST = 0xF0,
	M_EXT_LAST = 0xFD,
	M_COM											/* COMment */
};

/* Error exit handler */
#define ERREXIT(msg)  return -1;

typedef struct jpeg_component_info
{
	uint8_t component_id;
	uint8_t component_index;
	uint8_t h_samp_factor;
	uint8_t v_samp_factor;
	uint8_t quant_tbl_no;
	uint8_t dc_tbl_no;
	uint8_t ac_tbl_no;
	uint16_t width_in_blocks;
	uint16_t height_in_blocks;
} jpeg_component_info;

typedef struct jpeg_decompressor
{
	char *ptr;
	char *data;
	uint64_t length;
	uint8_t data_precision;
	uint16_t image_height;
	uint16_t image_width;
	uint8_t num_components;
	uint16_t restart_interval; 
	uint32_t max_h_samp_factor;
	uint32_t max_v_samp_factor;
	jpeg_component_info comp_info[MAX_COMPS_IN_SCAN];

	/* updated each scan */
	uint8_t first_scan;
	uint8_t comps_in_scan;
	uint8_t component_sel;
	uint8_t coding_table;
	uint8_t ss;
	uint8_t se;
	uint8_t Ah;
	uint8_t Al;
	jpeg_component_info cur_comp_info[MAX_COMP_INFO];
	uint8_t MCUs_per_row;
	uint8_t MCUs_rows_in_scan;
} jpeg_decompressor;


static int eof(jpeg_decompressor *d)
{
	return (d->ptr >= d->data + d->length);
}

static uint8_t read_byte(jpeg_decompressor *d)
{
	uint8_t temp;

	temp = (*d->ptr);
	d->ptr++;
	//printf("%s: 0x%02x\n", __func__, temp);
	return temp;
}

/* MSB order */
static uint16_t read_short(jpeg_decompressor *d)
{
	uint16_t temp3;
	uint8_t temp1, temp2;

	temp1 = *d->ptr;
	d->ptr++;
	temp2 = *d->ptr;
	d->ptr++;

	temp3 = (temp1 << 8) | temp2;
	//printf("%s: 0x%04x\n", __func__, temp3);
	return temp3;
}

static void skip_bytes(jpeg_decompressor *d, int count)
{
	//printf("Skipping %i bytes\n", count);
	if (d->ptr + count > d->data + d->length)
		d->ptr = d->data + d->length;
	else
		d->ptr+=count;
}

/* Skip over an unknown or uninteresting variable-length marker */
static int skip_variable(jpeg_decompressor *d)
{
  unsigned short length;

  /* Get the marker parameter length count */
  length = read_short(d);
  /* Length includes itself, so must be at least 2 */
  if (length < 2)
    ERREXIT("Erroneous JPEG marker length");
  length -= 2;
  /* Skip over the remaining bytes */
	skip_bytes(d, length);

	return 0;
}

static int next_marker(jpeg_decompressor *d)
{
	uint8_t byte;
	int marker;
	int discarded_bytes = 0;

	/* Find 0xFF byte; count and skip any non-FFs. */
	byte = read_byte(d);
	while (byte != 0xFF)
	{
		if (eof(d))
			return -1;

		discarded_bytes++;
		byte = read_byte(d);
	}

  /* Get marker code byte, swallowing any duplicate FF bytes.  Extra FFs
   * are legal as pad bytes, so don't count them in discarded_bytes.
   */
	do
	{
		if (eof(d))
			return -1;

		marker = read_byte(d);
	} while (marker == 0xFF);

  return marker;
}

static int read_jpeg_header(jpeg_decompressor *d)
{
	uint8_t c1=0, c2=0;

	if (!eof(d))
	{
		c1 = read_byte(d);
		c2 = read_byte(d);
	}
	if (c1 != 0xFF || c2 != M_SOI)
		printf("Not JPEG: %i %i\n", c1, c2);

	printf("Got JPEG marker!\n");

	return 0;
}

static void process_SOFn(int marker, jpeg_decompressor *d)
{
	unsigned int length;

	length = read_short(d);      /* usual parameter length count */
	printf("%s length=%u\n", __func__, length);

	d->data_precision = read_byte(d);
	d->image_height = read_short(d);
	d->image_width = read_short(d);
	d->num_components = read_byte(d);
	for (unsigned int i=0; i < d->num_components; i++)
	{
		d->comp_info[i].component_id = read_byte(d);
		uint8_t factor = read_byte(d);
		d->comp_info[i].h_samp_factor = factor << 4;
		d->comp_info[i].v_samp_factor = factor & 0xF;
		d->comp_info[i].quant_tbl_no = read_byte(d);
		printf("Index: %u H: %u V: %u Tq: %u\n", d->comp_info[i].component_id,
			d->comp_info[i].h_samp_factor,
			d->comp_info[i].v_samp_factor,
			d->comp_info[i].quant_tbl_no);
	}
}

static int process_reset_interval(jpeg_decompressor *d)
{
	unsigned int length;

	length = read_short(d);      /* usual parameter length count */
	if (length != 4)
		printf("Unexpected length in restart interval (saw %u)\n", length);

	d->restart_interval = read_short(d);
	printf("restart interval 0x%x\n", d->restart_interval);

	return 0;
}

/* F.2.2.3 Decode */
static int decode_ecs(jpeg_decompressor *d)
{
	uint8_t ss = read_byte(d);
	
	return 0;
}

static int process_scan(jpeg_decompressor *d)
{
	unsigned int length;

	// read the scan header (SOS)
	length = read_short(d);      /* usual parameter length count */
	printf("%s length=%u\n", __func__, length);

	d->comps_in_scan = read_byte(d);

	printf("Num components: %u\n", d->comps_in_scan);
	for (uint8_t i=0; i < d->comps_in_scan; i++)
	{
		d->cur_comp_info[i].component_id = read_byte(d);
		uint8_t tdta = read_byte(d);
		d->cur_comp_info[i].dc_tbl_no = tdta >> 4;
		d->cur_comp_info[i].ac_tbl_no = tdta & 0xF;
		
		printf("%u: selector = %u DC entropy table = %u AC entropy table = %u\n", i,
			d->cur_comp_info[i].component_id,
			d->cur_comp_info[i].dc_tbl_no,
			d->cur_comp_info[i].ac_tbl_no);
	}
	d->ss = read_byte(d);
	d->se = read_byte(d);
	uint8_t A = read_byte(d);
	d->Ah = A >> 4;
	d->Al = A & 0xF;
	printf("ss=%u se=%u Ah=%u Al=%u\n", d->ss, d->se, d->Ah, d->Al);

	if (d->first_scan)
	{
		d->first_scan = 0;
		d->max_h_samp_factor = 1;
		d->max_v_samp_factor = 1;

		for (uint8_t i=0; i < d->comps_in_scan; i++)
		{
			d->max_h_samp_factor = MAX(d->max_h_samp_factor, d->comp_info[i].h_samp_factor);
			d->max_v_samp_factor = MAX(d->max_v_samp_factor, d->comp_info[i].v_samp_factor);
		}
	}

	if (d->comps_in_scan > 1)
	{
		d->MCUs_per_row = d->image_width / d->max_h_samp_factor * DCTSIZE;
		d->MCUs_rows_in_scan = d->image_height / d->max_v_samp_factor * DCTSIZE;
		printf("MCUs per row: %u\n", d->MCUs_per_row);
	}
	// after the SOS there are ECS segments
	// each entropy-coded segment except the last one shall contain 'reset_interval' MCUs
/*
	int found = 0;
	while (!found)
	{
		int marker = next_marker();
		switch (marker)
		{
		case -1:
			return -1;

		case M_RST_FIRST ... M_RST_LAST:
			printf("Found reset %u @ 0x%lx\n", marker - M_RST_FIRST, ptr - data);
			break;

		default:
			printf("Found marker 0x%x @ 0x%lx\n", marker, ptr - data);
		}
	}
*/
	decode_ecs(d);

	return 0;
}

static int find_frame(jpeg_decompressor *d)
{
	int found = 0;

	printf("Looking for SOF\n");

	while (!found)
	{
		int marker = next_marker(d);
		switch (marker)
		{
		case -1:
			printf("Error: EOF while looking for SOF\n");
			return -1;

		case M_SOF0 ... M_SOF3:                /* Baseline */
			printf("SOF non-differential huffman\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_SOF5 ... M_SOF7:
			printf("SOF differential huffman\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_SOF9 ... M_SOF11:
			printf("SOF non-differential arithmetic\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_SOF13 ... M_SOF15:
			printf("SOF differential arithmetic\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_EOI:                 /* in case it's a tables-only JPEG stream */
			printf("Error: Hit EOI while looking for frame\n");
			return -1;

		case M_DRI:
			printf("Found reset interval\n");
			process_reset_interval(d);
			break;

		default:                    /* Anything else just gets skipped */
			skip_variable(d);          /* we assume it has a parameter count... */
		}
	}

	return 0;
}

static int find_scan(jpeg_decompressor *d)
{
	int marker;

	printf("Looking for SOS\n");

	/* Scan miscellaneous markers until we reach SOS. */
	for (;;)
	{
		marker = next_marker(d);
		switch (marker)
		{
		case -1:
			printf("reading past EOF\n");
			return -1;

		case M_RST_FIRST ... M_RST_LAST:
			printf("Found reset %u @ 0x%lx\n", marker - M_RST_FIRST, d->ptr - d->data);
			return -1;

		case M_SOS:
			printf("Found SOS\n");
			return process_scan(d);

		case M_EOI:                 /* in case it's a tables-only JPEG stream */
			printf("Error: Hit EOI while looking for scan\n");
			return -1;

		case M_APP_FIRST ... M_APP_LAST:
			skip_variable(d);
			break;

		default:                    /* Anything else just gets skipped */
			skip_variable(d);          /* we assume it has a parameter count... */
			break;
		}
	}

	return -2;
}

void jpeg_cpu_scale(uint64_t file_length, char *buffer)
{
	jpeg_decompressor decompressor;

	decompressor.data = buffer;
	decompressor.ptr = decompressor.data;
	decompressor.length = file_length;

	read_jpeg_header(&decompressor);

	// there should be a single frame in the buffer
	if (find_frame(&decompressor) == 0)
	{
		printf("Size: %u x %u\n", decompressor.image_width, decompressor.image_height);
		printf("Precision: %u bits\nNum components: %u\n",
			decompressor.data_precision, decompressor.num_components);
	}

	// the frame contains multiple scans
	decompressor.first_scan = 1;
	find_scan(&decompressor);
}
