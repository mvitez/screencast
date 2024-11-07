#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libpostproc/postprocess.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "ssdp.h"
#include "alsa.h"

#define AUFRAMELEN 1024

struct opt
{
    int fps, bitrate, width, height, local_port;
    char recdevice[100];
} opt;

void strcpysafechars(char *dst, const char *src)
{
    while (*src)
    {
        if (isalnum((unsigned char)*src))
            *dst++ = *src;
        else if (*src == ' ')
            *dst++ = '_';
        else
            *dst++ = '.';
        src++;
    }
    *dst = 0;
}

char **get_stream_items()
{
    Display *display = XOpenDisplay(NULL);
    Atom actualType;
    int format;
    unsigned long numItems;
    unsigned long bytesAfter;
    char **items = 0;
    unsigned char *data = 0;
    Window *list;
    char *windowName;

    if (!display)
    {
        fprintf(stderr, "Cannot open display\n");
        return 0;
    }
    Window rootWindow = RootWindow(display, DefaultScreen(display));
    Atom atom = XInternAtom(display, "_NET_CLIENT_LIST", 1);

    int status = XGetWindowProperty(display, rootWindow, atom, 0L, (~0L), 0, AnyPropertyType, &actualType, &format, &numItems, &bytesAfter, &data);
    list = (Window *)data;

    if (status >= Success && numItems)
    {
        items = (char **)malloc(sizeof(char *) * (numItems + 2));
        int n = 0;
        items[n++] = strdup("Desktop");
        for (int i = 0; i < numItems; ++i)
        {
            status = XFetchName(display, list[i], &windowName);
            printf("%d/%ld %s\n", i, numItems, windowName);
            if (status >= Success && windowName)
            {
                if (*windowName && strcmp(windowName, "Desktop"))
                    items[n++] = strdup(windowName);
                XFree(windowName);
            }
        }
        items[n] = 0;
    }
    XFree(data);
    XCloseDisplay(display);
    return items;
}

// Find the DOSBox Window so we can do cool stuff with it!
Window findWindowByName(Display *display, const char *name)
{
    Window rootWindow = RootWindow(display, DefaultScreen(display));
    if (!strcmp(name, "Desktop"))
    {
        printf("Returned rootWindow\n");
        return rootWindow;
    }
    Atom atom = XInternAtom(display, "_NET_CLIENT_LIST", 1);
    Atom actualType;
    int format;
    unsigned long numItems;
    unsigned long bytesAfter;

    unsigned char *data = 0;
    Window *list;
    char *windowName;

    int status = XGetWindowProperty(display, rootWindow, atom, 0L, (~0L), 0, AnyPropertyType, &actualType, &format, &numItems, &bytesAfter, &data);
    list = (Window *)data;

    if (status >= Success && numItems)
    {
        for (int i = 0; i < numItems; ++i)
        {
            char safename[300];
            status = XFetchName(display, list[i], &windowName);
            if (windowName)
                strcpysafechars(safename, windowName);
            else
                *safename = 0;
            if (status >= Success && windowName)
            {
                if (!strcmp(safename, name))
                {
                    printf("Returned window %s\n", windowName);
                    Window w = (Window)list[i];
                    XFree(windowName);
                    XFree(data);
                    return w;
                }
                XFree(windowName);
            }
        }
    }
    fprintf(stderr, "No window found\n");
    XFree(data);
    return 0;
}

int write_packet(void *opaque, uint8_t *buf, int buf_size)
{
    return write((int)(size_t)opaque, buf, buf_size);
}

struct ctx
{
    AVFormatContext *output_ctx;
    AVCodecContext *videoenc_ctx, *audioenc_ctx;
    AVStream *video_stream, *audio_stream;
    AVFrame *frame, *auframe;
    AVPacket *packet, *aupacket;
    uint8_t *avio_ctx_buffer;
    struct SwsContext *sws;
    int iwidth, iheight;
};

struct ctx *open_encoder(int csk, int width, int height, int owidth, int oheight, int fps, int bitrate, int abitrate)
{
    struct ctx *ctx;

    ctx = (struct ctx *)calloc(1, sizeof(*ctx));
    ctx->iwidth = width;
    ctx->iheight = height;
    // Create the output MPEG-2 TS file
    if (avformat_alloc_output_context2(&ctx->output_ctx, NULL, "mpegts", 0) < 0)
    {
        fprintf(stderr, "Failed to allocate output context\n");
        free(ctx);
        return 0;
    }

    // Create the video encoder context
    ctx->videoenc_ctx = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_H264));
    if (!ctx->videoenc_ctx)
    {
        fprintf(stderr, "Failed to allocate encoder context\n");
        avformat_free_context(ctx->output_ctx);
        free(ctx);
        return 0;
    }

    // Set the video encoder parameters
    ctx->videoenc_ctx->width = owidth;
    ctx->videoenc_ctx->height = oheight;
    ctx->videoenc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->videoenc_ctx->time_base = (AVRational){1, fps};
    ctx->videoenc_ctx->framerate = (AVRational){fps, 1};
    ctx->videoenc_ctx->bit_rate = bitrate;
    // videoenc_ctx->max_b_frames = 0; // To reduce latency

    AVDictionary *options = NULL;

    // Set the tune
    if (av_dict_set(&options, "tune", "zerolatency", 0) < 0)
    {
        fprintf(stderr, "Error setting libx264 tune\n");
        av_dict_free(&options);
    }

    // Open the video encoder
    if (avcodec_open2(ctx->videoenc_ctx, avcodec_find_encoder(AV_CODEC_ID_H264), &options) < 0)
    {
        fprintf(stderr, "Failed to open video encoder\n");
        av_dict_free(&options);
        avcodec_free_context(&ctx->videoenc_ctx);
        avformat_free_context(ctx->output_ctx);
        free(ctx);
        return 0;
    }
    av_dict_free(&options);
    // Add the video stream to the output context
    ctx->video_stream = avformat_new_stream(ctx->output_ctx, NULL);
    if (!ctx->video_stream)
    {
        fprintf(stderr, "Failed to create output stream\n");
        avcodec_free_context(&ctx->videoenc_ctx);
        avformat_free_context(ctx->output_ctx);
        free(ctx);
        return 0;
    }
    avcodec_parameters_from_context(ctx->video_stream->codecpar, ctx->videoenc_ctx);
    ctx->video_stream->time_base = (AVRational){1, 90000};

    if (abitrate)
    {
        // Create the audio encoder context
        ctx->audioenc_ctx = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_AAC));
        if (!ctx->audioenc_ctx)
        {
            fprintf(stderr, "Failed to allocate encoder context\n");
            avcodec_free_context(&ctx->videoenc_ctx);
            avformat_free_context(ctx->output_ctx);
            free(ctx);
            return 0;
        }

        ctx->audioenc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        ctx->audioenc_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
        ctx->audioenc_ctx->channels = 2;
        ctx->audioenc_ctx->time_base = (AVRational){1, 48000};
        ctx->audioenc_ctx->sample_rate = 48000;
        ctx->audioenc_ctx->bit_rate = abitrate;

        // Open the audio encoder
        if (avcodec_open2(ctx->audioenc_ctx, avcodec_find_encoder(AV_CODEC_ID_AAC), 0) < 0)
        {
            fprintf(stderr, "Failed to open audio encoder\n");
            avcodec_free_context(&ctx->audioenc_ctx);
            avcodec_free_context(&ctx->videoenc_ctx);
            avformat_free_context(ctx->output_ctx);
            free(ctx);
            return 0;
        }
        // Add the audio stream to the output context
        ctx->audio_stream = avformat_new_stream(ctx->output_ctx, NULL);
        if (!ctx->audio_stream)
        {
            fprintf(stderr, "Failed to create audio output stream\n");
            avcodec_free_context(&ctx->audioenc_ctx);
            avcodec_free_context(&ctx->videoenc_ctx);
            avformat_free_context(ctx->output_ctx);
            free(ctx);
            return 0;
        }
        avcodec_parameters_from_context(ctx->audio_stream->codecpar, ctx->audioenc_ctx);
        ctx->audio_stream->time_base = (AVRational){1, 90000};
        ctx->auframe = av_frame_alloc();
        ctx->auframe->format = AV_SAMPLE_FMT_FLTP;
        ctx->auframe->nb_samples = AUFRAMELEN;
        ctx->auframe->channel_layout = AV_CH_LAYOUT_STEREO;
        ctx->auframe->pts = 0;
        av_frame_get_buffer(ctx->auframe, 32);
        ctx->aupacket = av_packet_alloc();
    }

    ctx->avio_ctx_buffer = (uint8_t *)av_malloc(4096);

    ctx->output_ctx->pb = avio_alloc_context(ctx->avio_ctx_buffer, 4096, 1, (void *)(size_t)csk, 0, write_packet, 0);
    /*if (avio_open(&ctx->output_ctx->pb, "test.ts", AVIO_FLAG_WRITE) < 0) // For debugging
    {
        fprintf(stderr, "Failed to open output file: %s\n", "test.ts");
        avcodec_free_context(&ctx->videoenc_ctx);
        avformat_free_context(ctx->output_ctx);
        free(ctx);
        return 1;
    }*/
    if (avformat_init_output(ctx->output_ctx, 0) < 0)
    {
        fprintf(stderr, "Failed to initialize output\n");
        avio_context_free(&ctx->output_ctx->pb);
        avcodec_free_context(&ctx->videoenc_ctx);
        if (ctx->audioenc_ctx)
            avcodec_free_context(&ctx->audioenc_ctx);
        avformat_free_context(ctx->output_ctx);
        free(ctx);
        return 0;
    }

    // Open the output file
    ctx->frame = av_frame_alloc();
    if (!ctx->frame)
    {
        fprintf(stderr, "Failed to allocate video frame\n");
        avio_context_free(&ctx->output_ctx->pb);
        avcodec_free_context(&ctx->videoenc_ctx);
        if (ctx->audioenc_ctx)
            avcodec_free_context(&ctx->audioenc_ctx);
        avformat_free_context(ctx->output_ctx);
        free(ctx);
        return 0;
    }
    ctx->frame->width = ctx->videoenc_ctx->width;
    ctx->frame->height = ctx->videoenc_ctx->height;
    ctx->frame->format = ctx->videoenc_ctx->pix_fmt;
    ctx->frame->pts = 0;
    av_frame_get_buffer(ctx->frame, 32);
    printf("open_encoder ok, w=%d h=%d\n", ctx->frame->width, ctx->frame->height);
    ctx->packet = av_packet_alloc();
    ctx->sws = sws_getContext(width, height, AV_PIX_FMT_BGRA, owidth, oheight, AV_PIX_FMT_YUV420P, SWS_BICUBIC | PP_CPU_CAPS_MMX | PP_CPU_CAPS_MMX2, 0, 0, 0);

    return ctx;
}

int sendframe(struct ctx *ctx, const void *data)
{
    int srcStride[1] = {ctx->iwidth * 4};
    sws_scale(ctx->sws, (const uint8_t *const *)&data, srcStride, 0, ctx->iheight, ctx->frame->data, ctx->frame->linesize);

    int ret = avcodec_send_frame(ctx->videoenc_ctx, ctx->frame);
    if (ret < 0)
        return 0;

    ret = avcodec_receive_packet(ctx->videoenc_ctx, ctx->packet);
    if (ret < 0)
    {
        // printf("pts=%ld, not processed, yet\n", ctx->frame->pts);
        ctx->frame->pts += 90000 / opt.fps;
        return 0;
    }
    ctx->packet->stream_index = ctx->video_stream->index;
    // printf("sendframe w=%d h=%d, pktsize=%d\n", ctx->frame->width, ctx->frame->height, ctx->packet->size);
    if (av_interleaved_write_frame(ctx->output_ctx, ctx->packet) < 0)
    {
        av_packet_unref(ctx->packet);
        return -1;
    }
    av_packet_unref(ctx->packet);

    // Update the frame timestamp
    ctx->frame->pts += 90000 / opt.fps;
    return 0;
}

int sendaudioframe(struct ctx *ctx, const short *data)
{
    int len = AUFRAMELEN;
    float *bufl = (float *)ctx->auframe->data[0];
    float *bufr = (float *)ctx->auframe->data[1];

    for (int i = 0; i < len; i++)
    {
        bufl[i] = data[2 * i] / 32768.0f;
        bufr[i] = data[2 * i + 1] / 32768.0f;
    }

    int ret = avcodec_send_frame(ctx->audioenc_ctx, ctx->auframe);
    if (ret < 0)
        return 0;

    ret = avcodec_receive_packet(ctx->audioenc_ctx, ctx->aupacket);
    if (ret < 0)
    {
        ctx->auframe->pts += 90000 * AUFRAMELEN / 48000;
        return 0;
    }
    ctx->aupacket->stream_index = ctx->audio_stream->index;
    // printf("ausendframe %d pktsize=%d pts=%ld\n", ctx->aupacket->stream_index, ctx->aupacket->size, ctx->aupacket->pts);
    if (av_interleaved_write_frame(ctx->output_ctx, ctx->aupacket) < 0)
    {
        av_packet_unref(ctx->aupacket);
        return -1;
    }
    av_packet_unref(ctx->aupacket);

    // Update the frame timestamp
    ctx->auframe->pts += 90000 * AUFRAMELEN / 48000;
    return 0;
}

void close_encoder(struct ctx *ctx)
{
    av_write_trailer(ctx->output_ctx);
    avio_context_free(&ctx->output_ctx->pb);
    av_freep(&ctx->avio_ctx_buffer);
    av_packet_free(&ctx->packet);
    av_frame_free(&ctx->frame);
    av_packet_free(&ctx->aupacket);
    av_frame_free(&ctx->auframe);
    avcodec_free_context(&ctx->videoenc_ctx);
    if (ctx->audioenc_ctx)
        avcodec_free_context(&ctx->audioenc_ctx);
    avformat_free_context(ctx->output_ctx);
    sws_freeContext(ctx->sws);
    free(ctx);
}

double seconds()
{
    static double s;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!s)
        s = ts.tv_sec + ts.tv_nsec * 1e-9;
    return ts.tv_sec + ts.tv_nsec * 1e-9 - s;
}

int error_handler(Display *display, XErrorEvent *event)
{
    fprintf(stderr, "XWindow error %d\n", event->error_code);
    return 0;
}

int serve(int sk, const char *name)
{
    Display *display = XOpenDisplay(NULL);
    Window w;
    struct ctx *ctx;
    void *au = 0;
    short aubuf[AUFRAMELEN * 2];

    if (!display)
    {
        fprintf(stderr, "Cannot open display\n");
        return -1;
    }
    XWindowAttributes wattr;
    XSetErrorHandler(error_handler);
    w = findWindowByName(display, name);
    if (!w)
    {
        fprintf(stderr, "Window not found\n");
        return -1;
    }

    XGetWindowAttributes(display, w, &wattr);

    printf("Window width=%d height=%d\n", wattr.width, wattr.height);
    const char *reply = "HTTP/1.1 200 OK\r\nContent-Type: video/MP2T\r\nConnection: close\r\n\r\n";
    send(sk, reply, strlen(reply), 0);
    if (*opt.recdevice)
        au = au_open_record(opt.recdevice, 48000, 2, AUFRAMELEN * 4, 0);
    ctx = open_encoder(sk, wattr.width, wattr.height, opt.width, opt.height, opt.fps, opt.bitrate, au ? 96000 : 0);
    if (ctx)
    {
        double start = seconds();
        uint64_t asamples = 0;
        unsigned frameno = 0;

        for (;;)
        {
            if (!XGetWindowAttributes(display, w, &wattr))
            {
                fprintf(stderr, "XGetWindowAttributes failed\n");
                break;
            }
            if (wattr.width != ctx->iwidth || wattr.height != ctx->iheight)
            {
                ctx->iwidth = wattr.width;
                ctx->iheight = wattr.height;
                sws_freeContext(ctx->sws);
                printf("Window changed dimensions to w=%d h=%d\n", wattr.width, wattr.height);
                ctx->sws = sws_getContext(wattr.width, wattr.height, AV_PIX_FMT_BGRA, opt.width, opt.height, AV_PIX_FMT_YUV420P, SWS_BICUBIC | PP_CPU_CAPS_MMX | PP_CPU_CAPS_MMX2, 0, 0, 0);
            }
            XImage *image = XGetImage(display, w, 0, 0, ctx->iwidth, ctx->iheight, AllPlanes, ZPixmap);
            if (!image)
            {
                fprintf(stderr, "XGetImage failed\n");
                break;
            }
            if (sendframe(ctx, image->data))
            {
                XDestroyImage(image);
                break;
            }
            frameno++;
            XDestroyImage(image);
            if (au)
            {
                int fail = 0;
                while (asamples / 48000.0 < (double)frameno / opt.fps)
                {
                    if (au_get(au, aubuf) < 0)
                    {
                        fail = 1;
                        break;
                    }
                    if (sendaudioframe(ctx, aubuf))
                    {
                        fail = 1;
                        break;
                    }
                    asamples += 1024;
                }
                if (fail)
                    break;
            }
            else
                while (seconds() < start + (double)frameno / opt.fps)
                    usleep(10000);
        }
        close_encoder(ctx);
    }
    else
        fprintf(stderr, "Error opening encoder\n");
    if (au)
        au_close(au);
    XCloseDisplay(display);
    return 0;
}

int main(int argc, char *argv[])
{
    opt = (struct opt){30, 2000000, 1920, 1080, 8080};
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-H") || !strcmp(argv[i], "--help"))
        {
            printf("Syntax: screencast [options..]\n");
            printf("        -H, --help                         Print this help\n");
            printf("        -f <fps>, --fps <fps>              Frames per second, default 30\n");
            printf("        -b <bitrate>, --bitrate <bitrate>  Bitrate, default 2000000\n");
            printf("        -w <width>, --width <width>        Output width, default 1920\n");
            printf("        -h <height>, --height <height>     Output height, default 1080\n");
            printf("        -p <port>, --port <port>           Local TCP port for the HTTP server, default 8080\n");
            printf("        -a <device>, --audiodev <device>   Name of the audio device for sending audio, default none\n");
            return 0;
        }
        else if ((!strcmp(argv[i], "-b") || !strcmp(argv[i], "--bitrate")) && i + 1 < argc)
            opt.bitrate = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fps")) && i + 1 < argc)
            opt.fps = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-w") || !strcmp(argv[i], "--width")) && i + 1 < argc)
            opt.width = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-h") || !strcmp(argv[i], "--height")) && i + 1 < argc)
            opt.height = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")) && i + 1 < argc)
            opt.local_port = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-a") || !strcmp(argv[i], "--audiodev")) && i + 1 < argc)
            strcpy(opt.recdevice, argv[++i]);
    }
    start_upnp_server(opt.local_port);
    return 0;
}
