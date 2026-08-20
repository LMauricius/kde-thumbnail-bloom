/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ui_thumbnailbloom_config.h"

#include <KCModule>

namespace ThumbnailBloom
{

/*! Settings page shown by System Settings for the Thumbnail Bloom effect. */
class ThumbnailBloomEffectConfig : public KCModule
{
    Q_OBJECT

public:
    ThumbnailBloomEffectConfig(QObject *parent, const KPluginMetaData &data);

    /*! Stores the settings and asks the running KWin to pick them up. */
    void save() override;

private:
    Ui::ThumbnailBloomEffectConfig m_ui;
};

} // namespace ThumbnailBloom
