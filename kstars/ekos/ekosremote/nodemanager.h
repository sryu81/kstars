/*
    SPDX-FileCopyrightText: 2023 Jasem Mutlaq <mutlaqja@ikarustech.com>

    Node Manager

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QObject>
#include <QMap>
#include <QtWebSockets/QWebSocketServer>

#include "node.h"

namespace EkosRemote
{
class NodeManager : public QObject
{
        Q_OBJECT

    public:
        explicit NodeManager(quint16 port);
        ~NodeManager() override;

        bool isConnected() const;

        typedef enum
        {
            Message = 1 << 0,
            Media   = 1 << 1
        } Channels;

        Node *message()
        {
            return m_Nodes[Message];
        }
        Node *media()
        {
            return m_Nodes[Media];
        }

        void disconnectNodes();

    Q_SIGNALS:
        void connected();
        void disconnected();

    private Q_SLOTS:
        void onNewConnection();
        void setConnected();
        void setDisconnected();

    private:
        QWebSocketServer *m_Server { nullptr };
        QMap<Channels, Node*> m_Nodes;
};
}
