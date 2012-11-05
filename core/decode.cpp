#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <stdio.h>
}

#include "compat.hpp"
#include "d2v.hpp"
#include "decode.hpp"
#include "gop.hpp"

using namespace std;

/*
 * AVIO seek function to handle GOP offsets and multi-file support
 * in libavformat without it knowing about it.
 */
static int64_t file_seek(void *opaque, int64_t offset, int whence)
{
    decodecontext *ctx = (decodecontext *) opaque;

    switch(whence) {
    case SEEK_SET: {
        /*
         * This mutli-file seek is likely very broken, but I don't
         * really care much, since it is only used in avformat_find_stream_info,
         * which does its job fine as-is.
         */
        int64_t real_offset = offset + ctx->orig_file_offset;
        int i;

        for(i = ctx->orig_file; i < ctx->cur_file; i++)
            real_offset -= ctx->file_sizes[i];

        while(real_offset > ctx->file_sizes[ctx->cur_file] && ctx->cur_file != ctx->files.size() - 1) {
            real_offset -= ctx->file_sizes[ctx->cur_file];
            ctx->cur_file++;
        }

        while(real_offset < 0 && ctx->cur_file) {
            ctx->cur_file--;
            real_offset += ctx->file_sizes[ctx->cur_file];
        }

        fseeko(ctx->files[ctx->cur_file], real_offset, SEEK_SET);

        return offset;
    }
    case AVSEEK_SIZE: {
        /*
         * Return the total filesize of all files combined,
         * adjusted for GOP offset.
         */
        int64_t size = -(ctx->orig_file_offset);
        int i;

        for(i = ctx->orig_file; i < ctx->file_sizes.size(); i++)
            size += ctx->file_sizes[i];

        return size;
    }
    default:
        /* Shouldn't need to support anything else for our use case. */
        cout << "Unsupported seek!" << endl;
        return -1;
    }
}

/*
 * AVIO packet reading function to handle GOP offsets and multi-file support
 * in libavformat without it knowing about it.
 */
static int read_packet(void *opaque, uint8_t *buf, int size)
{
    decodecontext *ctx = (decodecontext *) opaque;
    int ret;

    /*
     * If we read in less than we got asked for, and we're
     * not on the last file, then start reading seamlessly
     * on the next file.
     */
    ret = fread(buf, 1, size, ctx->files[ctx->cur_file]);
    if (ret < size && ctx->cur_file != ctx->files.size() - 1) {
        ctx->cur_file++;
        fseeko(ctx->files[ctx->cur_file], 0, SEEK_SET);
        fread(buf + ret, 1, size - ret, ctx->files[ctx->cur_file]);
    } else {
        return ret;
    }

    return size;
}

/* Conditionally free all memebers of decodecontext. */
void decodefreep(decodecontext **ctx)
{
    decodecontext *lctx = *ctx;
    int i;

    if (!lctx)
        return;

    av_freep(&lctx->in);
    av_free_packet(&lctx->inpkt);

    if (lctx->fctx) {
        if (lctx->fctx->pb)
            av_freep(&lctx->fctx->pb);

        avformat_close_input(&lctx->fctx);
    }

    for(i = 0; i < lctx->files.size(); i++)
        fclose(lctx->files[i]);

    lctx->files.clear();
    lctx->file_sizes.clear();

    if (lctx->avctx) {
        avcodec_close(lctx->avctx);
        av_freep(&lctx->avctx);
    }

    delete lctx->fakename;
    delete lctx;

    *ctx = NULL;
}

/* Initialize everything we can with regards to decoding */
decodecontext *decodeinit(d2vcontext *dctx)
{
    decodecontext *ret;
    int i, av_ret;

    ret = new decodecontext;

    /* Zero the context to aid in conditional freeing later. */
    memset(ret, 0, sizeof(*ret));

    /* Holds our "filename" we pass to libavformat. */
    ret->fakename = new string;

    /* Open each file and stash its size. */
    for(i = 0; i < dctx->num_files; i++) {
        FILE *in;
        int64_t size;

        in = fopen(dctx->files[i].c_str(), "rb");
        if (!in) {
            cout << "Cannot open file: " << dctx->files[i] << endl;
            goto fail;
        }

        fseeko(in, 0, SEEK_END);
        size = ftello(in);
        fseeko(in, 0, SEEK_SET);

        ret->file_sizes.push_back(size);
        ret->files.push_back(in);
    }

    /*
     * Register all of our demuxers, parsers, and decoders.
     * Ideally, to create a smaller binary, we only enable the
     * following:
     *
     * Demuxers: mpegvideo, mpegps, mpegts.
     * Parsers: mpegvideo, mpegaudio.
     * Decoders: mpeg1video, mpeg2video.
     */
    avcodec_register_all();
    av_register_all();

    /* Set the correct decoder. */
    if (dctx->mpeg_type == 1) {
        ret->incodec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    } else if (dctx->mpeg_type == 2) {
        ret->incodec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    } else {
        cout << "Invalid MPEG Type." << endl;
        goto fail;
    }

    /* Allocate the codec's context. */
    ret->avctx = avcodec_alloc_context3(ret->incodec);
    if (!ret->avctx) {
        cout << "Cannot allocate AVCodecContext." << endl;
        goto fail;
    }

    /* Set the IDCT algorithm. */
    ret->avctx->idct_algo = dctx->idct_algo;

    /* Open it. */
    av_ret = avcodec_open2(ret->avctx, ret->incodec, NULL);
    if (av_ret < 0) {
        cout << "Cannot open decoder." << endl;
        goto fail;
    }

    /* Allocate the scratch buffer for our custom AVIO context. */
    ret->in = (uint8_t *) av_malloc(32 * 1024);
    if (!ret->in) {
        cout << "Cannot alloc inbuf." << endl;
        goto fail;
    }

    return ret;

fail:
    decodefreep(&ret);
    return NULL;
}

int decodeframe(int frame_num, d2vcontext *ctx, decodecontext *dctx, AVFrame *out)
{
    frame f;
    gop g;
    int i, j, av_ret, offset;
    bool next;

    /* Get our frame and the GOP its in. */
    f = ctx->frames[frame_num];
    g = ctx->gops[f.gop];

    /*
     * The offset is how many frames we have to decode from our
     * current position in order to get to the frame we want.
     * The initial offset is obtaiend during the parsing of the
     * D2V file, but it may be more in an open GOP situation,
     * which we handle below.
     */
    offset = f.offset;

    /*
     * If we're in a open GOP situation, then start decoding
     * from the previous GOP (one at most is needed), and adjust
     * out offset accordingly.
     */
    if (!(g.info & GOP_FLAG_CLOSED) && (f.gop - 1 > 0)) {
        int n = frame_num;
        frame t = ctx->frames[n];

        g = ctx->gops[f.gop - 1];

        /*
         * Find the offset of the last frame in the
         * previous GOP and add it to our offset.
         */
        while(t.offset)
            t = ctx->frames[--n];

        t = ctx->frames[--n];

        /*
         * Subtract one from the offset to compensate for
         * libavcodec delay, I think.
         */
        offset += t.offset - 1;
    }

    /*
     * Check if we're decoding linearly, and if the GOP
     * of the current frame and previous frame are either
     * the same, or also linear. If so, we can decode
     * linearly.
     */
    next = (dctx->last_gop == f.gop || dctx->last_gop == f.gop - 1) && dctx->last_frame == frame_num - 1;

    /* Skip GOP initialization if we're decoding linearly. */
    if (!next) {
        /* Free out format and AVIO contexts from the previous seek. */
        if (dctx->fctx) {
            if (dctx->fctx->pb)
                av_freep(&dctx->fctx->pb);

            avformat_close_input(&dctx->fctx);
        }

        /* Seek to our GOP offset and stash the info. */
        fseeko(dctx->files[g.file], g.pos, SEEK_SET);
        dctx->orig_file_offset = g.pos;
        dctx->orig_file        = g.file;
        dctx->cur_file         = g.file;

        /* Allocate format context. */
        dctx->fctx = avformat_alloc_context();
        if (!dctx->fctx) {
            cout << "Cannot allocate AVFormatContext." << endl;
            goto dfail;
        }

        /*
         * Find the demuxer for our input type, and also set
         * the "filename" that we pass to libavformat when
         * we open the demuxer with our custom AVIO context.
         */
        if (ctx->stream_type == ELEMENTARY) {
            dctx->fctx->iformat = av_find_input_format("mpegvideo");
            *dctx->fakename      = "fakevideo.m2v";
        } else if (ctx->stream_type == PROGRAM) {
            dctx->fctx->iformat = av_find_input_format("mpeg");
            *dctx->fakename      = "fakevideo.vob";
        } else if (ctx->stream_type == TRANSPORT) {
            dctx->fctx->iformat = av_find_input_format("mpegts");
            *dctx->fakename      = "fakevideo.ts";
        } else {
            cout << "Unsupported format." << endl;
            goto dfail;
        }

        /*
         * Initialize out custom AVIO context that libavformat
         * will use instead of a file. It uses our custom packet
         * reading and seeking functions that transparently work
         * with our indexed GOP offsets and multiple files.
         */
        dctx->fctx->pb = avio_alloc_context(dctx->in, 32 * 1024, 0, dctx, read_packet, NULL, file_seek);

        /* Open the demuxer. */
        av_ret = avformat_open_input(&dctx->fctx, (*dctx->fakename).c_str(), NULL, NULL);
        if (av_ret < 0) {
            cout << "Cannot open buffer in libavformat: " << av_ret << endl;
            goto dfail;
        }

        /*
         * Flush the buffers of our codec's context so we
         * don't need to re-initialize it.
         */
        avcodec_flush_buffers(dctx->avctx);

        /*
         * Call the abomination function to find out
         * how many streams we have.
         */
        avformat_find_stream_info(dctx->fctx, NULL);

        /* Free and re-initialize any existing packet. */
        av_free_packet(&dctx->inpkt);
        av_init_packet(&dctx->inpkt);
    }

    /* Loop over all of our streams and only process our video stream. */
    for(i = 0; i < dctx->fctx->nb_streams; i++) {
        if (dctx->fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            int o;

            /*
             * We don't need to read a new packet in if we are decoding
             * linearly, since it's still there from the previous iteration.
             */
            if (!next)
                av_read_frame(dctx->fctx, &dctx->inpkt);

            /* If we're decoding linearly, there is obviously no offset. */
            o = next ? 0 : offset;
            for(j = 0; j <= o; j++) {
                while(dctx->inpkt.stream_index != i) {
                    av_free_packet(&dctx->inpkt);
                    av_read_frame(dctx->fctx, &dctx->inpkt);
                }

                /*
                 * Loop until we have a whole frame, since there can be
                 * multi-packet frames.
                 */
                av_ret = 0;
                while(!av_ret) {
                    AVPacket orig = dctx->inpkt;

                    /*
                     * Decoding might not consume out whole packet, so
                     * stash the original packet info, loop until it
                     * is all consumed, and then restore it, it so
                     * we can free it properly.
                     */
                    while(dctx->inpkt.size > 0) {
                        int r = avcodec_decode_video2(dctx->avctx, out, &av_ret, &dctx->inpkt);

                        dctx->inpkt.size -= r;
                        dctx->inpkt.data += r;
                    }

                    dctx->inpkt = orig;
                    av_free_packet(&dctx->inpkt);

                    av_read_frame(dctx->fctx, &dctx->inpkt);
                }
            }
        }
    }

    /*
     * Stash the frame number we just decoded, and the GOP it
     * is a part of so we can check if we're decoding linearly
     * later on.
     */
    dctx->last_gop   = f.gop;
    dctx->last_frame = frame_num;

    return 0;

dfail:
    avformat_close_input(&dctx->fctx);
    return -1;
}
