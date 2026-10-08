#pragma once
#include <QObject>
#include <QHash>
#include <QByteArray>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QHostAddress;
class AppCore;
class InputManager;
class MpvController;

// Phone remote: a tiny HTTP server on the local network that serves a
// touch-friendly remote page (assets/remote/index.html) and a small JSON API
// behind it. Every button maps onto input the app already understands:
//
//   POST /api/action/<name>  → InputManager::tapAction (up/down/left/right/
//                              select/back/play_pause), i.e. the same
//                              synthesized key a gamepad press produces
//   POST /api/media/<KEY>    → MpvController::sendKey with one of the media key
//                              names scripts/mpv-media-keys.lua binds (no-op
//                              when nothing is playing)
//   GET  /api/status         → {"playing", "position", "duration"}
//
// Off by default (app setting "remote_server": "On"/"Off", port from
// "remote_server_port", default 2400), toggled live. Only answers peers on a
// private/loopback network, and POSTs must carry an X-240MP-Remote header so a
// random web page in the phone's browser can't drive the app cross-origin
// (a custom header forces a CORS preflight, which this server never approves).
class RemoteServer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString url READ url NOTIFY runningChanged)

public:
    explicit RemoteServer(const QString &appRoot, AppCore *appCore,
                          InputManager *input, MpvController *mpv,
                          QObject *parent = nullptr);
    ~RemoteServer() override;

    bool running() const;
    // http://<first LAN IPv4>:<port>/ while running, empty otherwise.
    QString url() const;

signals:
    void runningChanged();

private slots:
    void onAppSettingChanged(const QString &key, const QString &value);
    void onNewConnection();

private:
    void applySettings();
    void onReadyRead(QTcpSocket *socket);
    void handleRequest(QTcpSocket *socket, const QByteArray &method,
                       const QByteArray &path, const QHash<QByteArray, QByteArray> &headers);
    void respond(QTcpSocket *socket, int status, const QByteArray &contentType,
                 const QByteArray &body);
    static bool isLocalPeer(const QHostAddress &addr);

    QString        m_appRoot;
    AppCore       *m_appCore = nullptr;
    InputManager  *m_input   = nullptr;
    MpvController *m_mpv     = nullptr;
    QTcpServer    *m_server  = nullptr;
    quint16        m_port    = 0;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
