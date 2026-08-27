#include "MyInstantsClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>

MyInstantsClient::MyInstantsClient(QObject* parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)) {}

QString MyInstantsClient::defaultBaseUrl() {
    return QStringLiteral("https://myinstants-api.vercel.app");
}

QString MyInstantsClient::baseUrl() {
    QString url;

    const QByteArray override = qgetenv("MYINSTANTS_BASE_URL");
    if (!override.isEmpty()) {
        url = QString::fromUtf8(override);
    } else {
        const QString configured =
            QSettings().value("soundEffects/baseUrl").toString().trimmed();
        url = configured.isEmpty() ? defaultBaseUrl() : configured;
    }

    // Normalised for EVERY source, not just the settings one. Paths are
    // appended, so a URL typed or exported with a trailing slash would produce
    // "//search" — which some servers accept and others 404 on, making it the
    // kind of failure that looks like the service being down.
    url = url.trimmed();
    while (url.endsWith('/')) url.chop(1);
    return url;
}

void MyInstantsClient::setBaseUrl(const QString& url) {
    QSettings().setValue("soundEffects/baseUrl", url.trimmed());
}

void MyInstantsClient::search(const QString& query) {
    if (query.trimmed().isEmpty()) {
        emit listFinished({});
        return;
    }
    QUrl url(baseUrl() + "/search");
    QUrlQuery params;
    params.addQueryItem("q", query.trimmed());
    url.setQuery(params);
    runListRequest(url);
}

void MyInstantsClient::browse(Browse what) {
    QUrl url;
    QUrlQuery params;
    switch (what) {
        case Browse::Recent:
            url = QUrl(baseUrl() + "/recent");
            break;
        case Browse::Best:
            url = QUrl(baseUrl() + "/best");
            // The region parameter is required by this endpoint; "us" is a
            // neutral default and the results are global memes either way.
            params.addQueryItem("q", "us");
            url.setQuery(params);
            break;
        case Browse::Trending:
        default:
            url = QUrl(baseUrl() + "/trending");
            params.addQueryItem("q", "us");
            url.setQuery(params);
            break;
    }
    runListRequest(url);
}

void MyInstantsClient::runListRequest(const QUrl& url) {
    // Abort anything still running. Without this, a slow response to "boom" can
    // land after a fast one to "boom2" and repopulate the list with results for
    // a query the user has already moved on from.
    if (m_listReply) {
        m_listReply->abort();
        m_listReply = nullptr;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // Some community deployments reject requests with no user agent outright.
    request.setHeader(QNetworkRequest::UserAgentHeader, "GenieEditor/1.0");
    m_listReply = m_network->get(request);

    connect(m_listReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply* reply = m_listReply;
        if (!reply) return;
        m_listReply = nullptr;
        reply->deleteLater();

        if (reply->error() == QNetworkReply::OperationCanceledError) return; // superseded, not a failure

        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // The likely failures here are the service being down or having
            // moved, not the user doing something wrong — so the message points
            // at the thing they can actually change.
            if (status == 404) {
                emit listFailed("The sound service didn't recognise that request. "
                                "Its API may have changed \u2014 check the service URL in settings.");
            } else if (status == 429) {
                emit listFailed("The sound service is rate-limiting requests. Wait a moment, "
                                "or point the panel at your own deployment in settings.");
            } else {
                emit listFailed(QString("Couldn't reach the sound service: %1").arg(reply->errorString()));
            }
            return;
        }

        emit listFinished(parseListResponse(reply->readAll()));
    });
}

QVector<InstantSound> MyInstantsClient::parseListResponse(const QByteArray& json) {
    QVector<InstantSound> results;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return results;

    // The documented envelope is {status, author, data:[...]}, but a bare array
    // is accepted too — this is a scraper, and forks of it differ. Being liberal
    // about the wrapper costs three lines and avoids the panel appearing broken
    // against a slightly different deployment.
    QJsonArray items;
    const QJsonObject root = doc.object();
    if (root["data"].isArray()) items = root["data"].toArray();
    else if (root["results"].isArray()) items = root["results"].toArray();
    else return results;

    for (const QJsonValue& value : items) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();

        InstantSound sound;
        sound.id = item["id"].toString();
        sound.title = item["title"].toString().trimmed();
        sound.pageUrl = item["url"].toString();
        sound.mp3Url = item["mp3"].toString();

        // The audio URL is the one field with no fallback: an entry without it
        // can be neither previewed nor imported, so listing it would only
        // produce a row that does nothing when clicked.
        if (sound.mp3Url.isEmpty()) continue;
        if (sound.title.isEmpty()) sound.title = sound.id.isEmpty() ? "Untitled" : sound.id;

        results.push_back(sound);
    }
    return results;
}

void MyInstantsClient::fetchBytes(const QUrl& url, const QString& token) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, "GenieEditor/1.0");

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

void MyInstantsClient::cancelAll() {
    if (m_listReply) {
        m_listReply->abort();
        m_listReply = nullptr;
    }
}
