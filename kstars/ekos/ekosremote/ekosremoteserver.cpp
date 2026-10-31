/*
    SPDX-FileCopyrightText: 2018 Jasem Mutlaq <mutlaqja@ikarustech.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ekosremoteserver.h"
#include "ekos/manager.h"

#include "kspaths.h"
#include "ekos_debug.h"

namespace EkosRemote
{
Server::Server(Ekos::Manager *manager) : QDialog(manager), m_Manager(manager)
{
    setupUi(this);

    connect(closeB, &QPushButton::clicked, this, &Server::close);

    QPixmap im;
    if (im.load(KSPaths::locate(QStandardPaths::AppLocalDataLocation, "ekoslive.png")))
        leftBanner->setPixmap(im);

    connectionState->setPixmap(QIcon::fromTheme("state-offline").pixmap(QSize(64, 64)));
    statusLabel->setText(i18n("Listening on port %1", Port));

    m_NodeManager.reset(new NodeManager(Port));
    connect(m_NodeManager.get(), &NodeManager::connected, this, &Server::onConnected);
    connect(m_NodeManager.get(), &NodeManager::disconnected, this, &Server::onDisconnected);
    m_NodeManagers.append(m_NodeManager);

    m_Message = new Message(m_Manager, m_NodeManagers);
    connect(m_Message, &Message::connected, this, &Server::onConnected);
    connect(m_Message, &Message::disconnected, this, &Server::onDisconnected);

    m_Media = new Media(m_Manager, m_NodeManagers);
    connect(m_Media, &Media::connected, this, &Server::onConnected);
    connect(m_Media, &Media::disconnected, this, &Server::onDisconnected);

    // When the native livestacker starts/stops, suppress raw capture frames so
    // the App only receives the stacked results (delivered via PictureMonitor).
    connect(m_Message, &Message::liveStackingActiveChanged, m_Media, &Media::setLiveStackingActive);
    connect(m_Message, &Message::filePreviewRequested, m_Media, &Media::sendFilePreview);
}

Server::~Server()
{
    m_NodeManager->disconnectNodes();
}

void Server::onConnected()
{
    if (m_NodeManager->isConnected() && !m_isConnected)
    {
        m_isConnected = true;
        connectionState->setPixmap(QIcon::fromTheme("state-ok").pixmap(QSize(64, 64)));
        Q_EMIT connected();
    }
}

void Server::onDisconnected()
{
    if (!m_NodeManager->isConnected() && m_isConnected)
    {
        m_isConnected = false;
        connectionState->setPixmap(QIcon::fromTheme("state-offline").pixmap(QSize(64, 64)));
        Q_EMIT disconnected();
    }
}
}
