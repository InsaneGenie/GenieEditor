#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QByteArray>
#include <QSize>

class QNetworkAccessManager;

// One search result from Klipy.
struct KlipyGif {
    QString id;
    QString description;   // Klipy's own alt text — used as the accessible/tooltip label
    QString previewUrl;    // small, low-frame-rate GIF for the results grid
    QString fullUrl;       // the GIF actually imported when chosen
    QSize fullSize;
};

// Thin wrapper over Klipy's HTTP API.
//
// Klipy replaced Tenor here because Google shut the public Tenor API down on
// 30 June 2026 — new key registration had already stopped that January, so a
// Tenor integration written now could never have worked at all.
//
// Klipy requires an API key (free, from their Partner Panel). There is
// deliberately no key compiled in: a key embedded in a distributed binary is
// both a terms violation and trivially extractable, so it's asked for once and
// kept in QSettings.
//
// Parsing is a static pure function on purpose — it's the part most likely to
// break when the API's response shape shifts, and keeping it free of network
// and Qt object state means it can be tested against a captured payload without
// making a request.
class KlipyClient : public QObject {
    Q_OBJECT
public:
    explicit KlipyClient(QObject* parent = nullptr);

    void setApiKey(const QString& key) { m_apiKey = key; }
    QString apiKey() const { return m_apiKey; }
    bool hasApiKey() const { return !m_apiKey.trimmed().isEmpty(); }

    // Runs a search. Any request already in flight is aborted first, so typing
    // quickly can't have an older, slower response overwrite a newer one.
    void search(const QString& query, int limit = 30);

    // Fetches raw bytes (a preview thumbnail, or the full GIF being imported).
    // `token` is echoed back on completion so the caller can tell responses
    // apart without tracking replies itself.
    void fetchBytes(const QUrl& url, const QString& token);

    void cancelAll();

    // The search endpoint. Defaults to Klipy's, and can be redirected via the
    // KLIPY_BASE_URL environment variable — which exists so the panel can be
    // exercised against a local stand-in server without touching the network,
    // and incidentally allows routing through a corporate proxy.
    static QString searchEndpoint();

    // Parses a Klipy /search response. Returns an empty vector on malformed
    // input rather than throwing — a bad payload should show "no results", not
    // take the app down.
    static QVector<KlipyGif> parseSearchResponse(const QByteArray& json);

signals:
    void searchFinished(const QVector<KlipyGif>& results);
    void searchFailed(const QString& message);
    void bytesFetched(const QString& token, const QByteArray& data);
    void bytesFailed(const QString& token, const QString& message);

private:
    QNetworkAccessManager* m_network = nullptr;
    QString m_apiKey;
    class QNetworkReply* m_searchReply = nullptr;
};
