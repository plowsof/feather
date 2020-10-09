// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020, The Monero Project.

#include <QtCore>
#include <QScreen>
#include <QDesktopWidget>
#include <QProcess>
#include <QDesktopServices>
#include "utils/utils.h"
#include "utils/tor.h"
#include "appcontext.h"

QString Tor::torHost = "127.0.0.1";
quint16 Tor::torPort = 9050;

Tor::Tor(AppContext *ctx, QObject *parent) 
        : QObject(parent)
        , m_ctx(ctx)
        , m_checkConnectionTimer(new QTimer(this))
{
    connect(m_checkConnectionTimer, &QTimer::timeout, this, &Tor::checkConnection);

    this->torDir = QDir(m_ctx->configDirectory).filePath("tor");
    this->torDataPath = QDir(this->torDir).filePath("data");

    if (m_ctx->cmdargs->isSet("tor-port")) {
        Tor::torPort = m_ctx->cmdargs->value("tor-port").toUShort();
        this->localTor = true;
        if (!Utils::portOpen(Tor::torHost, Tor::torPort)) {
            this->errorMsg = QString("--tor-port was specified but no running Tor instance was found on port %1.").arg(QString::number(Tor::torPort));
        }
        return;
    }

    // Assume Tor is already running
    this->localTor = m_ctx->cmdargs->isSet("use-local-tor");
    if (this->localTor && !Utils::portOpen(Tor::torHost, Tor::torPort)) {
        this->errorMsg = "--use-local-tor was specified but no running Tor instance found.";
    }
    if (m_ctx->isTorSocks || m_ctx->isTails || m_ctx->isWhonix || Utils::portOpen(Tor::torHost, Tor::torPort))
        this->localTor = true;
    if (this->localTor) {
        return;
    }

    bool unpacked = this->unpackBins();
    if (!unpacked) {
        qCritical() << "Feather built without embedded Tor. Assuming --use-local-tor";
        this->localTor = true;
        return;
    }

    // Don't spawn Tor on default port to avoid conflicts
    Tor::torPort = 19450;
    if (Utils::portOpen(Tor::torHost, Tor::torPort)) {
        this->localTor = true;
        return;
    }

    qDebug() << "Using embedded tor instance";
    m_process.setProcessChannelMode(QProcess::MergedChannels);

    connect(&m_process, &QProcess::readyReadStandardOutput, this, &Tor::handleProcessOutput);
    connect(&m_process, &QProcess::errorOccurred, this, &Tor::handleProcessError);
    connect(&m_process, &QProcess::stateChanged, this, &Tor::stateChanged);
}

void Tor::stop() {
    m_process.kill();
}

void Tor::start() {
    if (this->localTor) {
        this->checkConnection();
        m_checkConnectionTimer->start(5000);
        return;
    }

    auto state = m_process.state();
    if (state == QProcess::ProcessState::Running || state == QProcess::ProcessState::Starting) {
        this->errorMsg = "Can't start Tor, already running or starting";
        return;
    }

    if (Utils::portOpen(Tor::torHost, Tor::torPort)) {
        this->errorMsg = QString("Unable to start Tor on %1:%2. Port already in use.").arg(Tor::torHost, Tor::torPort);
        return;
    }

    qDebug() << QString("Start process: %1").arg(this->torPath);

    m_restarts += 1;
    if (m_restarts > 4) {
        this->errorMsg = "Tor failed to start: maximum retries exceeded";
        return;
    }

    QStringList arguments;

    arguments << "--ignore-missing-torrc";
    arguments << "--SocksPort" << QString("%1:%2").arg(Tor::torHost, QString::number(Tor::torPort));
    arguments << "--TruncateLogFile" << "1";
    arguments << "--DataDirectory" << this->torDataPath;
    arguments << "--Log" << "notice";
    arguments << "--pidfile" << QDir(this->torDataPath).filePath("tor.pid");

    qDebug() << QString("%1 %2").arg(this->torPath, arguments.join(" "));

    m_process.start(this->torPath, arguments);
}

void Tor::checkConnection() {
    // We might not be able to connect to localhost if torsocks is used to start feather
    if (m_ctx->isTorSocks)
        this->setConnectionState(true);

    else if (m_ctx->isWhonix)
        this->setConnectionState(true);

    else if (m_ctx->isTails) {
        QStringList args = QStringList() << "--quiet" << "is-active" << "tails-tor-has-bootstrapped.target";
        int code = QProcess::execute("/bin/systemctl", args);

        this->setConnectionState(code == 0);
    }

    else if (Utils::portOpen(Tor::torHost, Tor::torPort))
        this->setConnectionState(true);

    else
        this->setConnectionState(false);
}

void Tor::setConnectionState(bool connected) {
    this->torConnected = connected;
    emit connectionStateChanged(connected);
}

void Tor::stateChanged(QProcess::ProcessState state) {
    if(state == QProcess::ProcessState::Running)
        qWarning() << "Tor started, awaiting bootstrap";
    else if (state == QProcess::ProcessState::NotRunning) {
        this->setConnectionState(false);

        if (m_stopRetries)
            return;

        QTimer::singleShot(1000, [=] {
            this->start();
        });
    }
}

void Tor::handleProcessOutput() {
    QByteArray output = m_process.readAllStandardOutput();
    this->torLogs.append(Utils::barrayToString(output));
    emit logsUpdated();
    if(output.contains(QByteArray("Bootstrapped 100%"))) {
        qDebug() << "Tor OK";
        this->setConnectionState(true);
    }

    qDebug() << output;
}

void Tor::handleProcessError(QProcess::ProcessError error) {
    if (error == QProcess::ProcessError::Crashed)
        qWarning() << "Tor crashed or killed";
    else if (error == QProcess::ProcessError::FailedToStart) {
        this->errorMsg = "Tor binary failed to start: " + this->torPath;
        this->m_stopRetries = true;
    }
}

bool Tor::unpackBins() {
    QString torFile;

    // @TODO: refactor for Mac OS - should compile Tor statically.
#if defined(Q_OS_MAC) && defined(DRONE)
    // Tor on Mac requires libevent.dylib, borrowed the executable from
    // the official Tor Browser release for now.
    QString libEvent = ":/tor/libevent-2.1.7.dylib";
    if (Utils::fileExists(libEvent)) {
        QFile e(libEvent);
        QFileInfo eventInfo(e);
        auto libEventPath = QDir(this->torDir).filePath(eventInfo.fileName());
        qDebug() << libEventPath;
        e.copy(libEventPath);
        e.close();
    }
#endif

#if defined(Q_OS_MAC) || defined(Q_OS_LINUX)
    torFile = ":/tor/tor";
#elif defined(Q_OS_WIN)
    torFile = ":/tor/tor.exe";
#endif

    if (!Utils::fileExists(torFile))
        return false;
    QFile f(torFile);
    QFileInfo fileInfo(f);
    this->torPath = QDir(this->torDir).filePath(fileInfo.fileName());
    qDebug() << this->torPath;
    f.copy(torPath);
    f.close();

#if defined(Q_OS_UNIX)
    QFile torBin(this->torPath);
    torBin.setPermissions(QFile::ExeGroup | QFile::ExeOther | QFile::ExeOther | QFile::ExeUser);
#endif

    return true;
}

networkPeer Tor::getPeerFromConfig(const QString &path) {
    // parse Tor bind addr from given Tor config
    QRegularExpression re("^SocksPort ([\\d|.|:]+)");

    networkPeer peer;
    peer.host = "127.0.0.1";
    peer.port = 9050;

    if(!Utils::fileExists(path)) {
        peer.active = Utils::portOpen(peer.host, peer.port);
        return peer;
    }

    for(const auto &line: Utils::fileOpen(path).split('\n')) {
        QRegularExpressionMatch match = re.match(line);
        if(!match.hasMatch())
            continue;

        QString match_group = match.captured(1);
        int host_idx = match_group.indexOf(':');
        if(host_idx >= 1){
            peer.host = match_group.mid(0, host_idx);
            QString port = match_group.mid(host_idx + 1);
            if(!Utils::isDigit(port))
                continue;

            peer.port = (quint16)port.toInt();
            qDebug() << "Parsed port from local Tor config";
            break;
        }

        if(Utils::isDigit(match_group)) {
            peer.port = (quint16)match_group.toInt();
            qDebug() << "Parsed port from local Tor config";
            break;
        }
    }

    peer.active = Utils::portOpen(peer.host, peer.port);
    return peer;
}

QString Tor::getVersion() {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(this->torPath, QStringList() << "--version");
    process.waitForFinished(-1);
    QString output = process.readAllStandardOutput();

    if(output.isEmpty()) {
        qWarning() << "Could not grab Tor version";
        return "";
    }
    QString version = output.split('\n').at(0);
    if(version.startsWith("Tor version")){
        return version;
    } else {
        qWarning() << "Could not parse Tor version";
        return "";
    }
}
