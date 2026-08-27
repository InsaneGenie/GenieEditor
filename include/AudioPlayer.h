#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <mpv/client.h>

// A headless (no video output) mpv instance used to drive a single audio
// track's playback independently of the video track. mpv only ever plays
// one loaded file's own embedded audio at a time, so genuinely independent
// per-track audio — where an uncut audio clip keeps playing right through a
// gap on the video track — requires one mpv instance per audio track, not
// just one shared with video.
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

    void loadFile(const QString& path);
    void seek(double seconds, bool exact = true);
    void play();
    void pause();
    bool isPaused() const;
    double positionSec() const;

    // Mutes mpv's own audio output while leaving decode/playback running —
    // deliberately NOT the same as pause(). Pausing would freeze this
    // player's position, requiring a re-seek (and losing sync with the
    // timeline) the moment it's unmuted. Muting keeps it perfectly in sync
    // the whole time; unmuting is instant.
    void setMuted(bool muted);

    // 0-100. Applies mpv's own "volume" property to this instance.
    void setVolume(int percent);

signals:
    void positionChanged(double seconds);
    void fileLoaded(double durationSeconds);

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    void command(const QVector<QString>& args);
    void handleMpvEvents();

    mpv_handle* m_mpv = nullptr;
    int m_pollTimerId = 0;
};
