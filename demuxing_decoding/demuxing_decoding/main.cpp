/*
 * Copyright (c) 2012 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * Demuxing and decoding example.
 *
 * Show how to use the libavformat and libavcodec API to demux and
 * decode audio and video data.
 * @example demuxing_decoding.c
 */

/**
 *  对视频进行解复用，然后对不同的流进行编码
 *  大致流程如下
 *  av_register_all             注册编解码器、格式等等
 *  avformat_open_input         打开媒体文件
 *  avformat_find_stream_info   查看媒体文件的流信息，看看是否有音频、视频、字幕等等
 *  av_find_best_stream(mediatype)         根据mediatype找到合适的流
 *  avcodec_find_decoder        为找到的 流 寻找合适的解码器
 *  avcodec_open2               为找到的 流 打开合适的解码器
 *  av_read_frame               读取待解码的数据到packet中,
 *                              packet.stream_index标识了这个数据是属于哪个流
 *  avcodec_decode_vedio2
 *  avcodec_decode_audio4       对音频或者视频进行解码
 */

#ifdef __cplusplus
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
extern "C"
{
#endif
#include "libavutil/imgutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/timestamp.h"
#include "libavformat/avformat.h"
#ifdef __cplusplus
}
#endif

static AVFormatContext *fmt_ctx = nullptr;
static AVCodecContext *video_dec_ctx = nullptr, *audio_dec_ctx = nullptr;
static int width, height;
static enum AVPixelFormat pix_fmt;
static AVStream *video_stream = nullptr, *audio_stream = nullptr;
static const char *src_filename = nullptr;
static const char *video_dst_filename = nullptr;
static const char *audio_dst_filename = nullptr;
static FILE *video_dst_file = nullptr;
static FILE *audio_dst_file = nullptr;

static uint8_t *video_dst_data[4] = {nullptr};
static int video_dst_linesize[4];
static int video_dst_bufsize;

static int video_stream_idx = -1, audio_stream_idx = -1;
static AVFrame *frame = nullptr;
static AVPacket pkt;
static int video_frame_count = 0;
static int audio_frame_count = 0;


static int refcount = 0;

static int decode_packet( int *got_frame, int cached )
{
    int ret = 0;
    int decoded = pkt.size;
    
    *got_frame = 0;
    
    if (pkt.stream_index == video_stream_idx) {
        // decode video frame
        ret = avcodec_decode_video2(video_dec_ctx, frame, got_frame, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error decoding video frame (%s)\n", av_err2str(ret));
            return ret;
        }
        
        if (*got_frame) {
            if (frame->width != width || frame->height != height ||
                frame->format != pix_fmt) {
                fprintf(stderr, "Error: Width, height and pixel format have to be "
                        "constant in a rawvideo file, but the width, height or "
                        "pixel format of the input video changed:\n"
                        "old: width = %d, height = %d, format = %s\n"
                        "new: width = %d, height = %d, format = %s\n",
                        width, height, av_get_pix_fmt_name(pix_fmt),
                        frame->width, frame->height,
                        av_get_pix_fmt_name((AVPixelFormat)frame->format));
                return -1;
            }
            
            printf("video_frame%s n:%d coded_n:%d pts:%s\n",
                   cached ? "(cached)" : "",
                   video_frame_count++, frame->coded_picture_number,
                   av_ts2timestr(frame->pts, &video_dec_ctx->time_base));
            
            /*  copy decoded frame to destination buffer;
             *  this is required since rawvideo expects non alignend data */
            av_image_copy(video_dst_data, video_dst_linesize, (const uint8_t **)(frame->data), frame->linesize, pix_fmt, width, height);
            fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
        }// end of got_frame
    }// end of decoding video
    else if (pkt.stream_index == audio_stream_idx){
        ret = avcodec_decode_audio4(audio_dec_ctx, frame, got_frame, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error decoding audio frame (%s)\n", av_err2str(ret));
            return ret;
        }
        
        decoded = FFMIN(ret, pkt.size);
        
        if (*got_frame) {
            size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample((AVSampleFormat)frame->format);
            printf("audio_frame%s n:%d nb_samples:%d pts:%s\n",
                   cached ? "(cached)" : "",
                   audio_frame_count++, frame->nb_samples,
                   av_ts2timestr(frame->pts, &audio_dec_ctx->time_base));
            
            /* Write the raw audio data samples of the first plane. This works
             * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
             * most audio decoders output planar audio, which uses a separate
             * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
             * In other words, this code will write only the first audio channel
             * in these cases.
             * You should use libswresample or libavfilter to convert the frame
             * to packed data. */
            fwrite(frame->extended_data[0], 1, unpadded_linesize, audio_dst_file);
        }
    }// end of decoding audio
    if (*got_frame && refcount)
        av_frame_unref(frame);
    
    return decoded;
}

//find stream id
//open decoder
static int open_codec_context( int *stream_idx, AVFormatContext *fmt_ctx, enum AVMediaType type )
{
    int ret = 0;
    int stream_index = 0;
    AVStream *st = NULL;
    AVCodecContext *dec_ctx = NULL;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;
    
    //根据 AVMediaType 来找到对应 流 的下标
    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    }
    else{
        //记录下标
        stream_index = ret;
        //获得流
        st = fmt_ctx->streams[stream_index];
        
        //find decoder for the stream
        dec_ctx = st->codec;
        dec = avcodec_find_decoder(dec_ctx->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }
        av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
        //打开编解码器
        if ((ret = avcodec_open2(dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        //返回 流 下标
        *stream_idx = stream_index;
        
    }
    return 0;
}

static int get_format_from_sample_fmt( const char **fmt, enum AVSampleFormat sample_fmt )
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;
    
    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }
    
    fprintf(stderr,
            "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}

int main( int argc, char* argv[] )
{
    int ret = 0;
    int got_frame = 0;
    
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s [-refcount] input_file video_output_file audio_output_file\n"
                "API example program to show how to read frames from an input file.\n"
                "This program reads frames from a file, decodes them, and writes decoded\n"
                "video frames to a rawvideo file named video_output_file, and decoded\n"
                "audio frames to a rawaudio file named audio_output_file.\n\n"
                "If the -refcount option is specified, the program use the\n"
                "reference counting frame system which allows keeping a copy of\n"
                "the data for longer than one decode call.\n"
                "\n", argv[0]);
        exit(1);
    }
    
    if (argc == 5 && !strcmp(argv[1], "-refcount")) {
        refcount = 1;
        argv++;
    }
    
    src_filename = argv[1];
    video_dst_filename = argv[2];
    audio_dst_filename = argv[3];

    
    // register all formats and codecs
    av_register_all();
    
    // open input file, and allocate format context
    // read the header
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Error: Could not open source file %s\n", src_filename);
        exit(1);
    }
    
    // retrieve stream information
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Error: Could not find stream information.\n");
        exit(1);
    }
    
    //open video codec context
    //find video stream
    //allocate data buffer for decoded image will be put
    if (open_codec_context(&video_stream_idx, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];
        video_dec_ctx = video_stream->codec;
        
        //open video destiantion file
        video_dst_file = fopen(video_dst_filename, "wb");
        if (!video_dst_file) {
            fprintf(stderr, "Error: Could not open destination file %s\n", video_dst_filename);
            ret = 1;
            goto end;
        }
        
        // allocate image where the decoded image will be put
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;
        //申请存放 图像 的内存空间，并且初始化linesize
        ret = av_image_alloc(video_dst_data, video_dst_linesize, width, height, pix_fmt, 1);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not allocate raw video buffer\n");
            goto end;
        }
        video_dst_bufsize = ret;
    }// end open video codec context
    
    //open audio codec context
    if (open_codec_context(&audio_stream_idx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];
        audio_dec_ctx = audio_stream->codec;
        
        //open audio destination file
        audio_dst_file = fopen(audio_dst_filename, "wb");
        if (!audio_dst_file) {
            fprintf(stderr, "Error: Could not open destination file %s\n", audio_dst_filename);
            ret = 1;
            goto end;
        }
    }
    
    // dump input information
    av_dump_format(fmt_ctx, 0, src_filename, 0);
    
    if (!audio_stream || !video_stream) {
        fprintf(stderr, "Error: Could not find audio or video stream in the in put, aborting\n");
        ret = 1;
        goto end;
    }
    
    //allocate frame
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error: Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    // initialize packet, set data to NULL, let the dumuxer fill it
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    
    if (video_stream)
        printf("Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
    if (audio_stream)
        printf("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_dst_filename);
    
    // read frames from the file
    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        AVPacket orig_pkt = pkt;
        do{
            ret = decode_packet(&got_frame, 0);
            if (ret < 0) {
                break;
            }
            
            pkt.data += ret;
            pkt.size -= ret;
        }while (pkt.size > 0);
        av_free_packet(&orig_pkt);
    }
    
    // flush cached frames
    pkt.data = NULL;
    pkt.size = 0;
    
    do{
        decode_packet(&got_frame, 1);
    }while(got_frame);
    
    printf("Demuxing succeeded.\n");
    
    if (video_stream) {
        printf("Play the output video file with the command:\n"
               "ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
               av_get_pix_fmt_name(pix_fmt), width, height,
               video_dst_filename);
    }
    
    if (audio_stream) {
        enum AVSampleFormat sfmt = audio_dec_ctx->sample_fmt;
        int n_channels = audio_dec_ctx->channels;
        const char *fmt;
        
        if (av_sample_fmt_is_planar(sfmt)) {
            const char *packed = av_get_sample_fmt_name(sfmt);
            printf("Warning: the sample format the decoder produced is planar "
                   "(%s). This example will output the first channel only.\n",
                   packed ? packed : "?");
            sfmt = av_get_packed_sample_fmt(sfmt);
            n_channels = 1;
        }
        
        if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
            goto end;
        
        printf("Play the output audio file with the command:\n"
               "ffplay -f %s -ac %d -ar %d %s\n",
               fmt, n_channels, audio_dec_ctx->sample_rate,
               audio_dst_filename);
    }
    
end:
    avcodec_close(video_dec_ctx);
    avcodec_close(audio_dec_ctx);
    avformat_close_input(&fmt_ctx);
    if (video_dst_file)
        fclose(video_dst_file);
    if (audio_dst_file)
        fclose(audio_dst_file);
    av_frame_free(&frame);
    av_free(video_dst_data[0]);
    

    return 0;
}













































