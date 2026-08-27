#include "KlipyClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

KlipyClient::KlipyClient(QObject* parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)) {}

QString KlipyClient::searchEndpoint() {
    const QByteArray override = qgetenv("KLIPY_BASE_URL");
    if (!override.isEmpty()) return QString::fromUtf8(override);

    // Klipy's documented migration path from Tenor is to swap the host and keep
    // the rest of the call identical: tenor.googleapis.com -> api.klipy.com.
    // That compatibility surface is used deliberately rather than Klipy's native
    // /api/v1/{key}/gifs/search route, because the compat request and response
    // objects are the ones Klipy publishes in full — the native response shape
    // isn't something to guess at. parseSearchResponse below accepts either
    // envelope, so moving to the native route later is a one-line change here.
    return "https://api.klipy.com/v2/search";
}

void KlipyClient::search(const QString& query, int limit) {
    if (!hasApiKey()) {
        emit searchFailed("No Klipy API key has been set.");
        return;
    }
    if (query.trimmed().isEmpty()) {
        emit searchFinished({});
        return;
    }

    // Abort anything still running. Without this, a slow response to "cat" can
    // land after a fast one to "cats" and repopulate the grid with results for
    // a query the user has already moved on from.
    if (m_searchReply) {
        m_searchReply->abort();
        m_searchReply = nullptr;
    }

    QUrl url(searchEndpoint());
    QUrlQuery params;
    params.addQueryItem("key", m_apiKey);
    params.addQueryItem("q", query.trimmed());
    params.addQueryItem("limit", QString::number(limit));
    // Only the two formats actually used, which keeps the response small.
    params.addQueryItem("media_filter", "tinygif,gif");
    // Filtered by default. This is a tool people will point at a shared screen,
    // and an unfiltered image search is a bad default for that.
    params.addQueryItem("contentfilter", "medium");
    params.addQueryItem("client_key", "video_editor");
    url.setQuery(params);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    m_searchReply = m_network->get(request);

    connect(m_searchReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply* reply = m_searchReply;
        if (!reply) return;
        m_searchReply = nullptr;
        reply->deleteLater();

        if (reply->error() == QNetworkReply::OperationCanceledError) return; // superseded, not a failure

        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // 401/403 almost always means the key is wrong or not yet active
            // is not yet active, which is worth saying outright rather than
            // reporting as a generic network error.
            if (status == 401 || status == 403) {
                emit searchFailed("Klipy rejected the API key. Check the key in your "
                                  "Klipy Partner Panel.");
            } else if (status == 429) {
                // Worth calling out separately: a Klipy TEST key is capped at
                // 100 calls/hour, which is easy to hit while trying things out
                // and looks like a broken integration if reported generically.
                emit searchFailed("Klipy rate limit reached. Test keys allow 100 requests "
                                  "per hour \u2014 wait a little, or request production "
                                  "access in the Partner Panel.");
            } else {
                emit searchFailed(reply->errorString());
            }
            return;
        }

        emit searchFinished(parseSearchResponse(reply->readAll()));
    });
}

void KlipyClient::fetchBytes(const QUrl& url, const QString& token) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_network->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, token] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::OperationCanceledError) return;
        if (reply->error() != QNetworkReply::NoError) {
            emit bytesFailed(token, reply->errorString());
            return;
        }
        emit bytesFetched(token, reply->readAll());
    });
}

void KlipyClient::cancelAll() {
    if (m_searchReply) {
        m_searchReply->abort();
        m_searchReply = nullptr;
    }
}

QVector<KlipyGif> KlipyClient::parseSearchResponse(const QByteArray& json) {
    QVector<KlipyGif> results;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return results;

    // Klipy's Tenor-compatible responses use "results"; its native endpoints
    // wrap the list in "data". Accepting both means the endpoint can be changed
    // without touching the parser.
    const QJsonObject root = doc.object();
    QJsonArray items = root.value("results").toArray();
    if (items.isEmpty()) {
        const QJsonValue data = root.value("data");
        items = data.isArray() ? data.toArray() : data.toObject().value("data").toArray();
    }
    for (const QJsonValue& itemValue : items) {
        const QJsonObject item = itemValue.toObject();
        QJsonObject formats = item.value("media_formats").toObject();
        if (formats.isEmpty()) formats = item.value("file").toObject(); // native envelope

        KlipyGif gif;
        gif.id = item.value("id").toString();
        if (gif.id.isEmpty()) gif.id = item.value("slug").toString();
        if (gif.id.isEmpty()) gif.id = QString::number(item.value("id").toInt());

        gif.description = item.value("content_description").toString();
        if (gif.description.isEmpty()) gif.description = item.value("title").toString();

        const QJsonObject preview = formats.value("tinygif").toObject();
        const QJsonObject full = formats.value("gif").toObject();

        gif.previewUrl = preview.value("url").toString();
        gif.fullUrl = full.value("url").toString();

        // Fall back rather than dropping the result: Klipy does not guarantee
        // every format exists for every GIF, and one missing size is no reason
        // to hide an otherwise usable result.
        if (gif.previewUrl.isEmpty()) gif.previewUrl = gif.fullUrl;
        if (gif.fullUrl.isEmpty()) gif.fullUrl = gif.previewUrl;
        if (gif.fullUrl.isEmpty()) continue; // nothing usable at all

        const QJsonArray dims = full.value("dims").toArray();
        if (dims.size() == 2) gif.fullSize = QSize(dims[0].toInt(), dims[1].toInt());

        results.push_back(gif);
    }
    return results;
}
