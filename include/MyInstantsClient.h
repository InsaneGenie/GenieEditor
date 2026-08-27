#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QByteArray>
#include <QUrl>

class QNetworkAccessManager;

// One sound effect from MyInstants.
struct InstantSound {
    QString id;      // e.g. "vine-boom-sound-70972"
    QString title;   // display name, as uploaded
    QString pageUrl; // the myinstants.com page, for attribution and "open original"
    QString mp3Url;  // the audio itself
};

// Thin wrapper over a MyInstants JSON API.
//
// IMPORTANT: MyInstants publishes no official API. This talks to a third-party
// service that scrapes the site and re-serves it as JSON. Two consequences are
// baked into the design here rather than discovered later:
//
//  1. The endpoint is NOT hardcoded. It defaults to the public instance but is
//     overridable in settings, because a free community service on someone
//     else's hosting can change shape, rate-limit, or vanish — and when it does,
//     the fix should be pasting a different URL rather than editing and
//     rebuilding. The service is MIT-licensed and self-hostable, so pointing
//     this at your own deployment is a supported path.
//
//  2. Parsing is defensive and tolerant. A scraper's output tracks someone
//     else's HTML, so fields go missing without warning. A malformed or
//     partially-populated payload yields fewer results, never a crash.
//
// Separately, and worth knowing before building a workflow on it: the sounds on
// MyInstants are user-uploaded, and a large share are clips of films, television
// and games that the uploader did not own. This client does not and cannot
// establish that any given sound is cleared for use.
class MyInstantsClient : public QObject {
    Q_OBJECT
public:
    explicit MyInstantsClient(QObject* parent = nullptr);

    // What the panel can ask for. Search needs a query; the browse modes don't.
    enum class Browse { Trending, Recent, Best };

    // Runs a search. Any request already in flight is aborted first, so typing
    // quickly can't have an older, slower response overwrite a newer one.
    void search(const QString& query);

    // Fills the list without a query, so the panel opens with something in it
    // rather than an empty box.
    void browse(Browse what);

    // Fetches raw bytes (the mp3 being imported). `token` is echoed back on
    // completion so the caller can tell responses apart without tracking
    // replies itself.
    void fetchBytes(const QUrl& url, const QString& token);

    void cancelAll();

    // Base URL of the JSON service, without a trailing slash. Reads the
    // MYINSTANTS_BASE_URL environment variable first (which exists so the panel
    // can be exercised against a local stand-in server without touching the
    // network), then the user's setting, then the public default.
    static QString baseUrl();
    static void setBaseUrl(const QString& url);
    static QString defaultBaseUrl();

    // Parses a response envelope of the form {status, data:[{id,title,url,mp3}]}.
    // A static pure function on purpose: it's the part most likely to break when
    // the upstream shape shifts, so it can be tested against a captured payload
    // without making a request.
    static QVector<InstantSound> parseListResponse(const QByteArray& json);

signals:
    void listFinished(const QVector<InstantSound>& results);
    void listFailed(const QString& message);
    void bytesFetched(const QString& token, const QByteArray& data);
    void bytesFailed(const QString& token, const QString& message);

private:
    void runListRequest(const QUrl& url);

    QNetworkAccessManager* m_network = nullptr;
    class QNetworkReply* m_listReply = nullptr;
};
