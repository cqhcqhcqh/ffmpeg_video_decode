#include "decodethread.h"
#include <QDebug>
#include <QFile>

#ifdef Q_OS_MAC
#define IN_FILE "/Users/chengqihan/Desktop/in.h264"
#define OUT_FILE "/Users/chengqihan/Desktop/out.yuv"
#else
#define IN_FILE "C:\\Workspaces\\in.h264"
#define OUT_FILE "C:\\Workspaces\\out.yuv"
#endif
#define READ_H264_DATA_SIZE 4096
#define ERROR_BUF(res) \
    char errbuf[1024]; \
    av_strerror(res, errbuf, sizeof(errbuf)); \

DecodeThread::DecodeThread(QObject *parent) : QThread(parent) {
    connect(this, &QThread::finished, this, &QThread::deleteLater);
}

DecodeThread::~DecodeThread() {
    disconnect();
    requestInterruption();
    quit();
    wait();

    qDebug() << "DecodeThread::~DecodeThread()";
}

static int check_pix_fmt(const AVCodec *codec,
                         enum AVPixelFormat fmt)
{
    const enum AVPixelFormat *p = codec->pix_fmts;
    if (p == nullptr) return 1;
    while (*p != AV_PIX_FMT_NONE) {
         qDebug() << "fmt: " << av_get_pix_fmt_name(*p);
        if (*p == fmt)
            return 1;
        p++;
    }
    return 0;
}

int video_decode(AVCodecContext *ctx, AVFrame *frame, AVPacket *packet, QFile &out) {
    /// 这里只需要 send 一次（全量）
    int res = avcodec_send_packet(ctx, packet);
    if (res < 0) {
        ERROR_BUF(res);
        qDebug() << "avcodec_send_packet err:" << errbuf;
        return res;
    }
    qDebug() << "avcodec_send_packet success";

    /* read all the available output packets (in general there may be any
         * number of them */
    /// 这里需要批量的 receive？
    while (true) {
        res = avcodec_receive_frame(ctx, frame);
        // EAGAIN: output is not available in the current state - user must try to send input
        // AVERROR_EOF: the encoder has been fully flushed, and there will be no more output packets
        // 退出函数，重新走 send 流程
        if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
           return 0;
        } else if (res < 0) {
           ERROR_BUF(res);
           qDebug() << "avcodec_receive_frame error" << errbuf;
           return res;
        }
        qDebug() << "avcodec_receive_packet success receive frame line size: " << frame->linesize[0]
                 << "frame->height" << frame->height;
        out.write((char *) frame->data[0], frame->linesize[0] * frame->height);
        out.write((char *) frame->data[1], frame->linesize[1] * frame->height >> 1);
        out.write((char *) frame->data[2], frame->linesize[2] * frame->height >> 1);
    }
}

/// h264 => yuv
void DecodeThread::run() {
    VideoDecodeSpec inSpec;
    inSpec.file = IN_FILE;

    VideoDecodeSpec outSpec;
    outSpec.file = OUT_FILE;

    char h264FileData[READ_H264_DATA_SIZE + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    char *inData = h264FileData;
    int in_len = READ_H264_DATA_SIZE;

    const AVCodec *codec = nullptr;
    AVCodecContext *ctx = nullptr;
    AVCodecParserContext *parserCtx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    QFile h264(IN_FILE);
    QFile yuv(OUT_FILE);
    int res = 0;
    int eof = 0;

    codec = avcodec_find_decoder_by_name("h264");
    if (codec == nullptr) {
        qDebug() << "avcodec_find_encoder_by_name failure";
        return;
    }

    qDebug() << codec->name;

    if (!check_pix_fmt(codec, AV_PIX_FMT_YUV420P)) {
        qDebug() << "check_sample_fmt not support pcm fmt" << av_get_pix_fmt_name(AV_PIX_FMT_YUV420P);
        goto end;
    }

    parserCtx = av_parser_init(codec->id);
    if (parserCtx == nullptr) {
        qDebug() << "av_parser_init failure";
        return;
    }

    ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) {
        qDebug() << "avcodec_alloc_context3 failure";
        return;
    }

    res = avcodec_open2(ctx, codec, nullptr);
    if (res < 0) {
        ERROR_BUF(res);
        qDebug() << "avcodec_open2 errbuf" << errbuf << "res" << res;
        goto end;
    }

    if (yuv.open(QFile::WriteOnly) == 0) {
        qDebug() << "pcm open failure file: " << outSpec.file;
        goto end;
    }

    if (h264.open(QFile::ReadOnly) == 0) {
        qDebug() << "aac open failure file: " << inSpec.file;
        goto end;
    }
    /* frame containing input raw audio */
    frame = av_frame_alloc();
    if (frame == nullptr) {
        qDebug() << "av_frame_alloc failure";
        goto end;
    }

    packet = av_packet_alloc();
    if (packet == nullptr) {
        qDebug() << "av_packet_alloc failure";
        goto end;
    }

    do {
        in_len = h264.read(h264FileData, READ_H264_DATA_SIZE);
        inData = h264FileData;
        eof = !in_len;
        // 读取到文件尾部（读不到文件内容的时候），还需要将 parse 中的数据再 flush 一次
        while (in_len > 0 || eof) {
            int parse_len = av_parser_parse2(parserCtx,
                             ctx,
                             (uint8_t **) &packet->data,
                             &packet->size,
                             (uint8_t *) inData,
                             in_len,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

            inData += parse_len;
            in_len -= parse_len;
            qDebug() << "packet size" << packet->size << "parse_len" << parse_len;

            /// 将解码出来的 packet 写入到 frame 中，再将 frame 的数据写入 yuv 文件中
            int ret = packet->size > 0 ? video_decode(ctx, frame, packet, yuv) : 0;

            if (eof) {
                break;
            }

            if (ret < 0) {
                goto end;
            }
        }
    } while (!eof);

    /*
     * It can be NULL, in which case it is considered a flush packet.
     * This signals the end of the stream. If the encoder
     * still has packets buffered, it will return them after this
     * call. Once flushing mode has been entered, additional flush
     * packets are ignored, and sending frames will return
     * AVERROR_EOF.
    */
    qDebug() << "flush packet";
    video_decode(ctx, frame, nullptr, yuv);

    // 赋值输出参数
    outSpec.fmt = ctx->pix_fmt;
    outSpec.fps = ctx->framerate.num;
    outSpec.width = ctx->width;
    outSpec.height = ctx->height;
    qDebug() << "pix_fmt" << av_get_pix_fmt_name(ctx->pix_fmt)
             << "width" << ctx->width
             << "height" << ctx->height
             << "framerate num" << ctx->framerate.num
             << "framerate den" << ctx->framerate.den;
end:
    yuv.close();
    h264.close();
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    av_parser_close(parserCtx);
}
