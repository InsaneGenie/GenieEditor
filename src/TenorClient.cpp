#include "TenorClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

TenorClient::TenorClient(QObject* parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)) {}

QString TenorClient::searchEndpoint() {
    const QByteArray override = qgetenv("TENOR_BASE_URL");
    if (!override.isEmpty()) return QString::fromUtf8(override);
    return "https://tenor.googleapis.com/v2/search";
}

void TenorClient::search(const QString& query, int limit) {
    if (!hasApiKey()) {
        emit searchFailed("No Tenor API key has been set.");
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
            // 401/403 almost always means the key is wrong or the Tenor API
            // isn't enabled for it, which is worth saying outright rather than
            // reporting as a generic network error.
            if (status == 401 || status == 403) {
                emit searchFailed("Tenor rejected the API key. Check that the key is correct "
                                  "and that the Tenor API is enabled for it.");
            } else {
                emit searchFailed(reply->errorString());
            }
            return;
        }

        emit searchFinished(parseSearchResponse(reply->readAll()));
    });
}

void TenorClient::fetchBytes(const QUrl& url, const QString& token) {
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

void TenorClient::cancelAll() {
    if (m_searchReply) {
        m_searchReply->abort();
        m_searchReply = nullptr;
    }
}

QVector<TenorGif> TenorClient::parseSearchResponse(const QByteArray& json) {
    QVector<TenorGif> results;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return results;

    const QJsonArray items = doc.object().value("results").toArray();
    for (const QJsonValue& itemValue : items) {
        const QJsonObject item = itemValue.toObject();
        const QJsonObject formats = item.value("media_formats").toObject();

        TenorGif gif;
        gif.id = item.value("id").toString();
        gif.description = item.value("content_description").toString();

        const QJsonObject preview = formats.value("tinygif").toObject();
        const QJsonObject full = formats.value("gif").toObject();

        gif.previewUrl = preview.value("url").toString();
        gif.fullUrl = full.value("url").toString();

        // Fall back rather than dropping the result: Tenor doesn't guarantee
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
