#include "AudioPlayer.h"
#include "Project.h"
#include <algorithm>
#include <cmath>

#include <QByteArray>
#include <vector>
#include <stdexcept>

static void checkAudioMpvError(int status) {
    if (status < 0) {
        throw std::runtime_error(mpv_error_string(status));
    }
}

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
    m_mpv = mpv_create();
    if (!m_mpv) {
        throw std::runtime_error("failed to create mpv instance (audio)");
    }

    // No video at all — this instance exists purely to drive one audio
    // track's playback clock, independent of whatever the video track (or
    // any other audio track) is doing.
    mpv_set_option_string(m_mpv, "vid", "no");
    mpv_set_option_string(m_mpv, "vo", "null");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "hr-seek", "yes");

    checkAudioMpvError(mpv_initialize(m_mpv));

    m_pollTimerId = startTimer(16);
}

AudioPlayer::~AudioPlayer() {
    if (m_pollTimerId) killTimer(m_pollTimerId);
    if (m_mpv) mpv_terminate_destroy(m_mpv);
}

void AudioPlayer::command(const QVector<QString>& args) {
    std::vector<QByteArray> storage;
    std::vector<const char*> cargs;
    storage.reserve(args.size());
    cargs.reserve(args.size() + 1);
    for (const auto& a : args) {
        storage.push_back(a.toUtf8());
        cargs.push_back(storage.back().constData());
    }
    cargs.push_back(nullptr);
    mpv_command(m_mpv, cargs.data());
}

void AudioPlayer::loadFile(const QString& path) {
    command({"loadfile", path});
}

void AudioPlayer::seek(double seconds, bool exact) {
    command({"seek", QString::number(seconds, 'f', 3), "absolute",
              exact ? "exact" : "keyframes"});
}

void AudioPlayer::play() {
    const int flag = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, const_cast<int*>(&flag));
}

void AudioPlayer::pause() {
    const int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, const_cast<int*>(&flag));
}

bool AudioPlayer::isPaused() const {
    int flag = 0;
    mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
    return flag != 0;
}

void AudioPlayer::setMuted(bool muted) {
    const int flag = muted ? 1 : 0;
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, const_cast<int*>(&flag));
}

void AudioPlayer::setSpeed(double speed) {
    if (!m_mpv) return;
    double rate = std::clamp(speed, Clip::kMinSpeed, Clip::kMaxSpeed);
    if (std::fabs(rate - m_speed) < 1e-6) return; // only on actual change
    m_speed = rate;
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &rate);
}

void AudioPlayer::setVolume(int percent) {
    double vol = percent;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

double AudioPlayer::positionSec() const {
    double pos = 0.0;
    mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

void AudioPlayer::timerEvent(QTimerEvent*) {
    handleMpvEvents();
    emit positionChanged(positionSec());
}

void AudioPlayer::handleMpvEvents() {
    while (true) {
        mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) break;

        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            double dur = 0.0;
            mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
            emit fileLoaded(dur);
        }
    }
}
