#include "ThumbnailGenerator.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <vector>
#include <algorithm>

ThumbnailStrip ThumbnailGenerator::generate(const QString& path, int frameCount, int thumbW, int thumbH) {
    ThumbnailStrip result;
    if (frameCount <= 0) return result;

    AVFormatContext* fmtCtx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&fmtCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        return result;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    const AVCodec* decoder = nullptr;
    const int videoStreamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (videoStreamIndex < 0 || !decoder) {
        avformat_close_input(&fmtCtx);
        return result; // no video stream — nothing to visualize, not an error
    }

    AVStream* stream = fmtCtx->streams[videoStreamIndex];

    double durationSec = 0.0;
    if (fmtCtx->duration != AV_NOPTS_VALUE) {
        durationSec = static_cast<double>(fmtCtx->duration) / AV_TIME_BASE;
    } else if (stream->duration != AV_NOPTS_VALUE) {
        durationSec = stream->duration * av_q2d(stream->time_base);
    }
    if (durationSec <= 0.0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx
        || avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0
        || avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        if (codecCtx) avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    SwsContext* sws = sws_getContext(
        codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
        thumbW, thumbH, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    std::vector<uint8_t> rgbBuffer(static_cast<size_t>(thumbW) * thumbH * 3);
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer.data(),
                          AV_PIX_FMT_RGB24, thumbW, thumbH, 1);

    AVPacket* packet = av_packet_alloc();

    for (int i = 0; i < frameCount; ++i) {
        const double targetSec = durationSec * (static_cast<double>(i) / frameCount);
        const int64_t targetTs = static_cast<int64_t>(targetSec * AV_TIME_BASE);

        avformat_seek_file(fmtCtx, -1, INT64_MIN, targetTs, targetTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx);

        bool gotFrame = false;
        int attempts = 0;
        while (!gotFrame && attempts < 200 && av_read_frame(fmtCtx, packet) >= 0) {
            if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecCtx, packet) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    ++attempts;
                    const int64_t rawPts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : frame->best_effort_timestamp;
                    const double framePts = rawPts * av_q2d(stream->time_base);
                    if (framePts >= targetSec || attempts >= 200) {
                        sws_scale(sws, frame->data, frame->linesize, 0, codecCtx->height,
                                  rgbFrame->data, rgbFrame->linesize);
                        QImage img(rgbFrame->data[0], thumbW, thumbH, rgbFrame->linesize[0],
                                   QImage::Format_RGB888);
                        result.frames.push_back(img.copy()); // own the memory — rgbBuffer gets reused next loop
                        gotFrame = true;
                        break;
                    }
                }
            }
            av_packet_unref(packet);
        }

        if (!gotFrame) {
            // Couldn't land a frame near this timestamp (e.g. near EOF on an
            // unusual container) — push a blank placeholder rather than a
            // gap, which would desync the slice-mapping math at draw time.
            QImage blank(thumbW, thumbH, QImage::Format_RGB888);
            blank.fill(Qt::black);
            result.frames.push_back(blank);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&rgbFrame);
    av_frame_free(&frame);
    sws_freeContext(sws);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    result.durationSec = durationSec;
    return result;
}
