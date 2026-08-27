#include "MediaProbe.h"

extern "C" {
#include <libavformat/avformat.h>
}

double MediaProbe::probeDurationSeconds(const QString& path) {
    AVFormatContext* fmtCtx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();

    // avformat_open_input reads just the container header (fast) — this
    // does NOT decode any frames, so it's cheap enough to call on every
    // import without a worker thread.
    if (avformat_open_input(&fmtCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        return -1.0;
    }

    // Some containers (certain .mkv/.avi muxings) don't have a duration in
    // the header itself and need this deeper scan to compute it from the
    // stream's packet timestamps instead.
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return -1.0;
    }

    double durationSec = -1.0;
    if (fmtCtx->duration != AV_NOPTS_VALUE) {
        // fmtCtx->duration is in AV_TIME_BASE units (microseconds by
        // definition of AV_TIME_BASE), not the stream's own timebase.
        durationSec = static_cast<double>(fmtCtx->duration) / AV_TIME_BASE;
    }

    avformat_close_input(&fmtCtx);
    return durationSec;
}

MediaProbe::VideoInfo MediaProbe::probeVideoInfo(const QString& path) {
    VideoInfo info;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) return info;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return info;
    }

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;

        info.width = stream->codecpar->width;
        info.height = stream->codecpar->height;

        // r_frame_rate is the stream's real base rate; avg_frame_rate can be 0
        // for streams that never finished being analysed, so fall back rather
        // than dividing by zero.
        AVRational rate = stream->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = stream->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) info.fps = av_q2d(rate);
        break;
    }

    avformat_close_input(&fmt);
    return info;
}

bool MediaProbe::hasAudioStream(const QString& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        return true; // couldn't tell — see the header's note on failing safe
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return true;
    }

    bool found = false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            found = true;
            break;
        }
    }

    avformat_close_input(&fmt);
    return found;
}
