/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QRasterWindow>

namespace ThumbnailBloom
{

/*!
 * A transparent, focus-less window placed exactly on top of a thumbnail.
 *
 * A compositing effect only paints; it cannot receive pointer input unless it
 * grabs the pointer globally, which an always-on effect must not do. This
 * window, created inside the KWin process, is therefore used as the click
 * target of a thumbnail: it covers the thumbnail's rectangle, paints nothing at
 * all and reports the clicks it swallows through clicked(). The hover outline is
 * drawn by the effect instead, so that it stays in step with the animation.
 */
class ThumbnailOverlay : public QRasterWindow
{
    Q_OBJECT

public:
    ThumbnailOverlay();
    ~ThumbnailOverlay() override;

Q_SIGNALS:
    /*! The overlay, and with it the thumbnail below it, was clicked. */
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
};

} // namespace ThumbnailBloom
