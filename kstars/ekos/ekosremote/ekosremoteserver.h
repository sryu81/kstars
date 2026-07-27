/*
    SPDX-FileCopyrightText: 2018 Jasem Mutlaq <mutlaqja@ikarustech.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QDialog>
#include <memory>

#include "ekos/ekos.h"
#include "ekos/manager.h"

// EkosRemote communication channels
#include "message.h"
#include "media.h"
#include "nodemanager.h"

#include "ui_ekosremotedialog.h"

namespace EkosRemote
{
class Server : public QDialog, public Ui::EkosRemoteDialog
{
        Q_OBJECT
    public:
        explicit Server(Ekos::Manager *manager);
        ~Server() override;

        // Fixed listen port. The server is always on while KStars runs —
        // trusted local network, no auth, no enable/disable toggle.
        static constexpr quint16 Port = 9000;

        bool isConnected() const
        {
            return m_isConnected;
        }

        const QPointer<Message> &message()
        {
            return m_Message;
        }
        const QPointer<Media> &media()
        {
            return m_Media;
        }

    Q_SIGNALS:
        void connected();
        void disconnected();

    private Q_SLOTS:
        void onConnected();
        void onDisconnected();

    private:
        Ekos::Manager *m_Manager { nullptr };
        bool m_isConnected {false};

        QSharedPointer<NodeManager> m_NodeManager;
        // Message/Media take a vector of NodeManagers (a leftover of the old
        // Online/Offline dial-out setup) — kept as a single-element vector since
        // that constructor shape doesn't need to change for a single listener.
        QVector<QSharedPointer<NodeManager >> m_NodeManagers;

        QPointer<Message> m_Message;
        QPointer<Media> m_Media;
};
}
