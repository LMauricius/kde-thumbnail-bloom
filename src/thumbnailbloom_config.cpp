/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailbloom_config.h"
#include "thumbnailbloomconfig.h"

#include <KPluginFactory>

#include <QDBusConnection>
#include <QDBusMessage>

K_PLUGIN_CLASS(ThumbnailBloom::ThumbnailBloomEffectConfig)

namespace ThumbnailBloom
{

ThumbnailBloomEffectConfig::ThumbnailBloomEffectConfig(QObject *parent, const KPluginMetaData &data)
    : KCModule(parent, data)
{
    m_ui.setupUi(widget());
    addConfig(ThumbnailBloomConfig::self(), widget());
}

void ThumbnailBloomEffectConfig::save()
{
    KCModule::save();

    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                                                          QStringLiteral("/Effects"),
                                                          QStringLiteral("org.kde.kwin.Effects"),
                                                          QStringLiteral("reconfigureEffect"));
    message.setArguments({QStringLiteral("thumbnailbloom")});
    QDBusConnection::sessionBus().send(message);
}

} // namespace ThumbnailBloom

#include "thumbnailbloom_config.moc"
