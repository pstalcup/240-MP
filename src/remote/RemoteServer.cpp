#include "RemoteServer.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QDebug>

#include "AppCore.h"
#include "input/InputManager.h"
#include "player/MpvController.h"

namespace {
constexpr quint16 kDefaultPort   = 2400;
constexpr int     kMaxHeaderSize = 8 * 1024;   // anything bigger isn't our page talking

// mpv key names bound by scripts/mpv-media-keys.lua — the only keys the remote
// may send straight to mpv.
const QSet<QString> &allowedMediaKeys() {
    static const QSet<QString> keys = {
        QStringLiteral("PLAYPAUSE"), QStringLiteral("STOP"),
        QStringLiteral("FORWARD"),   QStringLiteral("REWIND"),
        QStringLiteral("NEXT"),      QStringLiteral("PREV"),
        QStringLiteral("VOLUME_UP"), QStringLiteral("VOLUME_DOWN"),
        QStringLiteral("MUTE"),
    };
    return keys;
}

QByteArray reasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    }
    return "Error";
}
}

RemoteServer::RemoteServer(const QString &appRoot, AppCore *appCore,
                           InputManager *input, MpvController *mpv, QObject *parent)
    : QObject(parent)
    , m_appRoot(appRoot)
    , m_appCore(appCore)
    , m_input(input)
    , m_mpv(mpv)
{
    if (m_appCore)
        connect(m_appCore, &AppCore::appSettingChanged, this, &RemoteServer::onAppSettingChanged);
    applySettings();
}

RemoteServer::~RemoteServer() = default;

bool RemoteServer::running() const {
    return m_server && m_server->isListening();
}

QString RemoteServer::url() const {
    if (!running())
        return QString();
    for (const QHostAddress &addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback()
            && isLocalPeer(addr))
            return QStringLiteral("http://%1:%2/").arg(addr.toString()).arg(m_port);
    }
    return QStringLiteral("http://localhost:%1/").arg(m_port);
}

void RemoteServer::onAppSettingChanged(const QString &key, const QString &value) {
    Q_UNUSED(value)
    if (key == QLatin1String("remote_server") || key == QLatin1String("remote_server_port"))
        applySettings();
}

// Starts, stops or re-binds the listener to match config. Called at startup and
// whenever either setting changes, so the toggle in Settings takes effect live.
void RemoteServer::applySettings() {
    const bool wanted = m_appCore
        && m_appCore->get_setting(QString(), QStringLiteral("remote_server")).toString()
               == QLatin1String("On");
    bool ok = false;
    int port = m_appCore
        ? m_appCore->get_setting(QString(), QStringLiteral("remote_server_port")).toInt(&ok)
        : 0;
    if (!ok || port <= 0 || port > 65535)
        port = kDefaultPort;

    const bool wasRunning = running();
    if (wasRunning && (!wanted || m_port != quint16(port))) {
        m_server->close();
        qInfo("[remote] Phone remote stopped");
    }
    if (wanted && !running()) {
        if (!m_server) {
            m_server = new QTcpServer(this);
            connect(m_server, &QTcpServer::newConnection, this, &RemoteServer::onNewConnection);
        }
        m_port = quint16(port);
        if (m_server->listen(QHostAddress::Any, m_port))
            qInfo("[remote] Phone remote listening at %s", qPrintable(url()));
        else
            qWarning("[remote] Could not listen on port %d: %s", port,
                     qPrintable(m_server->errorString()));
    }
    if (wasRunning != running())
        emit runningChanged();
}

void RemoteServer::onNewConnection() {
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        if (!isLocalPeer(socket->peerAddress())) {
            qWarning("[remote] Refusing non-local peer %s",
                     qPrintable(socket->peerAddress().toString()));
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

// Minimal HTTP/1.1: one request per connection (Connection: close). The page
// only ever sends bodiless GET/POSTs, so a request is complete at the blank line.
void RemoteServer::onReadyRead(QTcpSocket *socket) {
    auto it = m_buffers.find(socket);
    if (it == m_buffers.end())
        return;
    it->append(socket->readAll());

    const int headerEnd = it->indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (it->size() > kMaxHeaderSize)
            respond(socket, 431, "text/plain", "Too large\n");
        return;
    }

    const QList<QByteArray> lines = it->left(headerEnd).split('\n');
    m_buffers.remove(socket);

    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    if (requestLine.size() < 2) {
        respond(socket, 400, "text/plain", "Bad request\n");
        return;
    }
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines[i].indexOf(':');
        if (colon > 0)
            headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
    }

    QByteArray path = requestLine[1];
    const int query = path.indexOf('?');
    if (query >= 0)
        path.truncate(query);
    handleRequest(socket, requestLine[0], path, headers);
}

void RemoteServer::handleRequest(QTcpSocket *socket, const QByteArray &method,
                                 const QByteArray &path,
                                 const QHash<QByteArray, QByteArray> &headers) {
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        QFile page(m_appRoot + QStringLiteral("/assets/remote/index.html"));
        if (!page.open(QIODevice::ReadOnly)) {
            respond(socket, 500, "text/plain", "Remote page missing\n");
            return;
        }
        respond(socket, 200, "text/html; charset=utf-8", page.readAll());
        return;
    }

    // The app's own VCR font, so the page looks like the TV UI.
    if (method == "GET" && path == "/font.ttf") {
        QFile font(m_appRoot + QStringLiteral("/assets/fonts/VCR_OSD_MONO_1.001.ttf"));
        if (!font.open(QIODevice::ReadOnly)) {
            respond(socket, 404, "text/plain", "Not found\n");
            return;
        }
        respond(socket, 200, "font/ttf", font.readAll());
        return;
    }

    if (method == "GET" && path == "/api/status") {
        QJsonObject status;
        status["playing"]  = m_mpv && m_mpv->isRunning();
        status["position"] = m_mpv ? m_mpv->position() : 0;
        status["duration"] = m_mpv ? m_mpv->duration() : 0;
        respond(socket, 200, "application/json",
                QJsonDocument(status).toJson(QJsonDocument::Compact));
        return;
    }

    const bool isAction = path.startsWith("/api/action/");
    const bool isMedia  = path.startsWith("/api/media/");
    if (!isAction && !isMedia) {
        respond(socket, 404, "text/plain", "Not found\n");
        return;
    }
    if (method != "POST") {
        respond(socket, 405, "text/plain", "POST only\n");
        return;
    }
    if (!headers.contains("x-240mp-remote")) {
        respond(socket, 403, "text/plain", "Missing X-240MP-Remote header\n");
        return;
    }

    const QString name = QString::fromUtf8(path.mid(path.lastIndexOf('/') + 1));
    if (isAction) {
        if (!m_input || !m_input->tapAction(name)) {
            respond(socket, 404, "text/plain", "Unknown action\n");
            return;
        }
    } else {
        const QString key = name.toUpper();
        if (!m_mpv || !allowedMediaKeys().contains(key)) {
            respond(socket, 404, "text/plain", "Unknown media key\n");
            return;
        }
        m_mpv->sendKey(key);
    }
    respond(socket, 204, QByteArray(), QByteArray());
}

void RemoteServer::respond(QTcpSocket *socket, int status, const QByteArray &contentType,
                           const QByteArray &body) {
    m_buffers.remove(socket);
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasonPhrase(status) + "\r\n";
    if (!contentType.isEmpty())
        out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
           "Cache-Control: no-store\r\n"
           "Connection: close\r\n\r\n";
    out += body;
    socket->write(out);
    socket->disconnectFromHost();
}

// Loopback, RFC 1918, link-local, and IPv6 unique-local — i.e. the same LAN.
// (IPv4-mapped IPv6 peers are unwrapped first, since QTcpServer on Any reports
// IPv4 clients that way on dual-stack hosts.)
bool RemoteServer::isLocalPeer(const QHostAddress &peer) {
    QHostAddress addr = peer;
    bool isV4 = false;
    const quint32 v4 = addr.toIPv4Address(&isV4);
    if (isV4)
        addr = QHostAddress(v4);
    if (addr.isLoopback())
        return true;
    static const QList<QPair<QHostAddress, int>> subnets = {
        QHostAddress::parseSubnet(QStringLiteral("10.0.0.0/8")),
        QHostAddress::parseSubnet(QStringLiteral("172.16.0.0/12")),
        QHostAddress::parseSubnet(QStringLiteral("192.168.0.0/16")),
        QHostAddress::parseSubnet(QStringLiteral("169.254.0.0/16")),
        QHostAddress::parseSubnet(QStringLiteral("fc00::/7")),
        QHostAddress::parseSubnet(QStringLiteral("fe80::/10")),
    };
    for (const auto &subnet : subnets) {
        if (addr.isInSubnet(subnet))
            return true;
    }
    return false;
}
