/* EkosRemote Node

    SPDX-FileCopyrightText: 2023 Jasem Mutlaq <mutlaqja@ikarustech.com>

    Node Manager — listens for incoming WebSocket connections and routes them,
    by request path, to the matching Message/Media node.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "nodemanager.h"
#include "ekos_debug.h"

#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>

namespace EkosRemote
{

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
NodeManager::NodeManager(quint16 port)
{
    m_Nodes[Message] = new Node("message");
    m_Nodes[Media] = new Node("media");

    for (auto &node : m_Nodes)
    {
        connect(node, &Node::connected, this, &NodeManager::setConnected);
        connect(node, &Node::disconnected, this, &NodeManager::setDisconnected);
    }

    m_Server = new QWebSocketServer(QStringLiteral("EkosRemote"), QWebSocketServer::NonSecureMode, this);
    connect(m_Server, &QWebSocketServer::newConnection, this, &NodeManager::onNewConnection);

    if (!m_Server->listen(QHostAddress::Any, port))
        qCWarning(KSTARS_EKOS) << "EkosRemote: failed to listen on port" << port << ":" << m_Server->errorString();
    else
        qCInfo(KSTARS_EKOS) << "EkosRemote: listening on port" << port;
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
NodeManager::~NodeManager()
{
    m_Server->close();
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
void NodeManager::onNewConnection()
{
    while (m_Server->hasPendingConnections())
    {
        QWebSocket *socket = m_Server->nextPendingConnection();
        const QString path = socket->requestUrl().path();

        if (path == m_Nodes[Message]->path())
            m_Nodes[Message]->adoptSocket(socket);
        else if (path == m_Nodes[Media]->path())
            m_Nodes[Media]->adoptSocket(socket);
        else
        {
            qCWarning(KSTARS_EKOS) << "EkosRemote: rejecting connection to unknown path" << path;
            socket->close();
            socket->deleteLater();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
bool NodeManager::isConnected() const
{
    return std::all_of(m_Nodes.begin(), m_Nodes.end(), [](const auto & node)
    {
        return node->isConnected();
    });
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
void NodeManager::setConnected()
{
    auto node = qobject_cast<Node*>(sender());
    if (!node)
        return;

    // Only emit once all nodes are connected.
    if (isConnected())
        Q_EMIT connected();
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
void NodeManager::setDisconnected()
{
    auto node = qobject_cast<Node *>(sender());
    if (!node)
        return;

    // Only emit once all nodes are disconnected.
    if (isConnected() == false)
        Q_EMIT disconnected();
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
void NodeManager::disconnectNodes()
{
    for (auto &node : m_Nodes)
        node->disconnectServer();
}

}
