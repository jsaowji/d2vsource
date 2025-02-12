/*
 * VapourSynth D2V Plugin
 *
 * Copyright (c) 2012 Derek Buitenhuis
 *
 * This file is part of d2vsource.
 *
 * d2vsource is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * d2vsource is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with d2vsource; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include <cstdint>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "d2vsource.hpp"
#include "directrender.hpp"

#include <VapourSynth.h>
#include <VSHelper.h>

int VSGetBuffer(AVCodecContext *avctx, AVFrame *pic, int flag)
{
    d2vData *data = (d2vData *) avctx->opaque;

    if (!data->format_set) {
        switch(avctx->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            data->vi.format = data->api->getFormatPreset(pfYUV420P8, data->core);
            break;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUVJ422P:
            data->vi.format = data->api->getFormatPreset(pfYUV422P8, data->core);
            break;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
            data->vi.format = data->api->getFormatPreset(pfYUV444P8, data->core);
            break;
        case AV_PIX_FMT_YUV420P9:
            data->vi.format = data->api->getFormatPreset(pfYUV420P9, data->core);
            break;
        case AV_PIX_FMT_YUV422P9:
            data->vi.format = data->api->getFormatPreset(pfYUV422P9, data->core);
            break;
        case AV_PIX_FMT_YUV444P9:
            data->vi.format = data->api->getFormatPreset(pfYUV444P9, data->core);
            break;
        case AV_PIX_FMT_YUV420P10:
            data->vi.format = data->api->getFormatPreset(pfYUV420P10, data->core);
            break;
        case AV_PIX_FMT_YUV422P10:
            data->vi.format = data->api->getFormatPreset(pfYUV422P10, data->core);
            break;
        case AV_PIX_FMT_YUV444P10:
            data->vi.format = data->api->getFormatPreset(pfYUV444P10, data->core);
            break;
        default:
            return -1;
        }
        data->format_set = true;
    }

    VSData *userdata = new VSData();
    userdata->d2v      = (d2vData *) avctx->opaque;
    userdata->vs_frame = data->api->newVideoFrame(data->vi.format, data->aligned_width, data->aligned_height, NULL, data->core);

    pic->buf[0] = av_buffer_create(NULL, 0, VSReleaseBuffer, userdata, 0);
    if (!pic->buf[0])
        return -1;

    pic->opaque              = (void *) userdata->vs_frame;
    pic->extended_data       = pic->data;
    pic->width               = data->aligned_width;
    pic->height              = data->aligned_height;
    pic->format              = avctx->pix_fmt;
    pic->sample_aspect_ratio = avctx->sample_aspect_ratio;

    for(int i = 0; i < data->vi.format->numPlanes; i++) {
        pic->data[i]     = data->api->getWritePtr(userdata->vs_frame, i);
        pic->linesize[i] = data->api->getStride(userdata->vs_frame, i);
    }

    return 0;
}

void VSReleaseBuffer(void *opaque, uint8_t *data)
{
    VSData *userdata = (VSData *) opaque;

    userdata->d2v->api->freeFrame(userdata->vs_frame);
    delete userdata;
}
