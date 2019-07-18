// Original source:
// http://web.me.com/dhoerl/Home/Tech_Blog/Entries/2009/1/22_Revised_avcodec_sample.c.html
//

#include "write_frame.h"
#include <stdio.h>

static void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame);

bool WriteFrame(AVCodecContext *dec_ctx, AVFrame *frame, int frame_num)
{
    struct SwsContext *img_convert_ctx = NULL;

    // Allocate an AVFrame structure
    AVFrame *pFrameRGB = av_frame_alloc();
    if(pFrameRGB==NULL)
        return false;

    // Determine required buffer size and allocate buffer
    int numBytes = avpicture_get_size(AV_PIX_FMT_RGB24,
                                      dec_ctx->width,
                                      dec_ctx->height);

    uint8_t *buffer = (uint8_t*) malloc(numBytes);

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    avpicture_fill((AVPicture *)pFrameRGB,
                    buffer,
                    AV_PIX_FMT_RGB24,
                    dec_ctx->width,
                    dec_ctx->height);

	// Convert the image into YUV format that SDL uses
	if(img_convert_ctx == NULL) {
		img_convert_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, 
                                         dec_ctx->pix_fmt, 
                                         dec_ctx->width, dec_ctx->height,
                                         AV_PIX_FMT_RGB24, SWS_BICUBIC,
                                         NULL, NULL, NULL);

		if(img_convert_ctx == NULL) {
			fprintf(stderr, "Cannot initialize the conversion context!\n");
			exit(1);
		}
	}

	int ret = sws_scale(img_convert_ctx, frame->data, frame->linesize, 0, 
						dec_ctx->height, pFrameRGB->data, pFrameRGB->linesize);

    // Save the frame to disk
    SaveFrame(pFrameRGB, dec_ctx->width, dec_ctx->height, frame_num);

    // Free the RGB image
    free(buffer);
    av_free(pFrameRGB);

    return true;
}

static void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame)
{
    FILE *pFile;
    char szFilename[32];
    int  y;

    // Open file
    sprintf(szFilename, "frame%d.ppm", iFrame);
    pFile=fopen(szFilename, "wb");
    if(pFile==NULL)
        return;

    // Write header
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // Write pixel data
    for(y=0; y<height; y++)
        fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);

    // Close file
    fclose(pFile);
}