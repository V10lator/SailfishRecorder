/*
 * SailfishRecorder v0.1 - A screen recorer for the Jolla phone.
 * Copyright (C) 2015 Thomas "V10lator" Rohloff <v10lator@myway.de>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
 
 // Compiel with: gcc -march=native -O2 -pipe -fno-ident -std=c11 -Wl,--as-needed SailfishRecorder.c -lavcodec -lavutil -lavformat -lrt -o SailfishRecorder

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/time.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// Stolen from fbcat
#if !defined(le32toh)
	#if BYTE_ORDER == LITTLE_ENDIAN
		#define le32toh(x) (x)
	#else
		#include <byteswap.h>
		#define le32toh(x) bswap_32(x)
	#endif
#endif


static const char default_fbdev[] = "/dev/fb0";
static bool running = true;

void aborter(int);

// Stolen from fbcat
static inline unsigned char get_color(unsigned int pixel, const struct fb_bitfield *bitfield, uint16_t *colormap)
{
  return colormap[(pixel >> bitfield->offset) & ((1 << bitfield->length) - 1)] >> 8;
}

int main(int argc, const char **argv)
{
	// Print out infos
	printf("SailfishRecorder v0.1 - A screen recorer for the Jolla phone.\n");
	printf("Copyright (C) 2015 Thomas \"V10lator\" Rohloff <v10lator@myway.de>\n");
	printf("\n");
	printf("SailfishRecorder comes with ABSOLUTELY NO WARRANTY; for details\n");
	printf("see http://www.gnu.org/licenses/gpl-2.0.html\n\n");
	
	// Check args
	if(argc != 2)
	{
		printf("%s filename\n", argv[0]);
		return 1;
	}
	
	// Try to open output file
	FILE *file = fopen(argv[1], "wb");
	if(!file)
	{
		fprintf(stderr, "Cannot open %s\n", argv[1]);
		return 1;
	}
	
	// Get framebuffer file
	const char *fbdev_name = getenv("FRAMEBUFFER");
    if(fbdev_name == NULL || fbdev_name[0] == '\0')
		fbdev_name = default_fbdev;
	
	// Try to open framebuffer
	int fd = open(fbdev_name, O_RDONLY);
	if(fd == -1)
    {
		fprintf(stderr, "Cannot open %s\n", fbdev_name);
		fclose(file);
		return 1;
	}
	
	// Get infos from framebuffer
	struct fb_var_screeninfo info;
	struct fb_fix_screeninfo fix_info;
	if(ioctl(fd, FBIOGET_VSCREENINFO, &info) || ioctl(fd, FBIOGET_FSCREENINFO, &fix_info))
	{
		fprintf(stderr, "Cannot get infos from %s\n", fbdev_name);
		close(fd);
		fclose(file);
		return 1;
	}
	
	// Open codec for the output file
	AVCodec *codec = avcodec_find_encoder(CODEC_ID_H264);
	if(!codec)
	{
		fprintf(stderr, "Cannot find h264 codec (check your libav installation)\n");
		close(fd);
		fclose(file);
		return 1;
	}
	
	// Set video context
	AVCodecContext *context = avcodec_alloc_context3(codec);
	context->bit_rate = 400000;
	context->width = info.xres;
	context->height = info.yres;
	context->time_base = (AVRational){1,25};
	context->gop_size = 10;
	context->max_b_frames = 1;
	context->pix_fmt = PIX_FMT_VDPAU_H264;
	av_opt_set(context->priv_data, "preset", "slow", 0);
	
	// Open codec
	if(avcodec_open2(context, codec, NULL) < 0)
	{
		fprintf(stderr, "Cannot open h264 codec (check your libav installation)\n");
		av_free(context);
		close(fd);
		fclose(file);
		return 1;
	}
	
	// Allocate buffer
	int outbuf_size = 100000;
	uint8_t *outbuf = malloc(outbuf_size);
	if(outbuf == NULL)
	{
		fprintf(stderr, "Malloc failed!\n");
		avcodec_close(context);
		av_free(context);
		close(fd);
		fclose(file);
		return 1;
	}
	
	// Allocate frame
	int size = context->width * context->height;
	AVFrame *frame = avcodec_alloc_frame();
	frame->format = context->pix_fmt;
	frame->width = context->width;
	frame->height = context->height;
	
	// Stolen from fbcat
	uint16_t colormap_data[4][1 << 8];
	struct fb_cmap colormap =
	{
		0,
		1 << 8,
		colormap_data[0],
		colormap_data[1],
		colormap_data[2],
		colormap_data[3],
	};
	unsigned int i;
	for (i = 0; i < (1U << info.red.length); i++)
		colormap.red[i] = i * 0xffff / ((1 << info.red.length) - 1);
	for (i = 0; i < (1U << info.green.length); i++)
		colormap.green[i] = i * 0xffff / ((1 << info.green.length) - 1);
	for (i = 0; i < (1U << info.blue.length); i++)
		colormap.blue[i] = i * 0xffff / ((1 << info.blue.length) - 1);
	
	// Set handler for Strg + C
	if(signal(SIGINT, aborter) == SIG_ERR)
	{
		fprintf(stderr, "Signal handling error!\n");
		avcodec_close(context);
		av_free(context);
		av_freep(&frame->data[0]);
		av_free(&frame); // av_frame_free missing?!?
		close(fd);
		fclose(file);
		return 1;
	}
	
	const size_t mapped_length = fix_info.line_length * (info.yres + info.yoffset);
	unsigned char image[context->width][context->height][3];
	unsigned int offset = fix_info.line_length + info.xoffset * 4;
	struct timespec start={0,0}, end={0,0}, zzz={0,0};
	printf("Recording... ");
	while(running)
	{
		clock_gettime(CLOCK_MONOTONIC, &start);
		unsigned char *video_memory = mmap(NULL, mapped_length, PROT_READ, MAP_SHARED, fd, 0);
		for (int y = 0; y < info.yres; y++)
		{
			const unsigned char *current;
			current = video_memory + (y + info.yoffset) * offset;
			for (int x = 0; x < info.xres; x++)
			{
				unsigned int pixel = 0;
				pixel = le32toh(*((uint32_t *) current));
				current += 4;
				//TODO: Instead of writing in an array, write into frame
				image[x][y][0] = get_color(pixel, &info.transp, colormap.red);
				image[x][y][1] = get_color(pixel, &info.blue, colormap.green);
				image[x][y][2] = get_color(pixel, &info.green, colormap.blue);
			}
		}
		munmap(video_memory, mapped_length);
		clock_gettime(CLOCK_MONOTONIC, &end);
		zzz.tv_nsec = 40000 - (end.tv_nsec - start.tv_nsec);
		nanosleep(&zzz);
	}
	
	// Write end of video
	outbuf[0] = 0x00;
	outbuf[1] = 0x00;
	outbuf[2] = 0x01;
	outbuf[3] = 0xb7;
	fwrite(outbuf, 1, 4, file);
	printf("Done! Have a nice day.\n");
	// Clean up everything
	close(fd);
	fclose(file);
	avcodec_close(context);
	av_free(context);
	av_freep(&frame->data[0]);
	av_free(&frame);
	free(outbuf);
}

void aborter(int signal)
{
	running = false;
}
