/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QRasterWindow>

namespace ThumbnailBloom
{

/*!
 * A transparent, focus-less window that swallows every pointer event on it.
 *
 * A compositing effect only paints; it cannot receive pointer input unless it
 * grabs the pointer globally, which an always-on effect must not do. Internal
 * windows created inside the KWin process are the way around that: KWin hit
 * tests them above the ordinary windows and offers them the event first, so one
 * of these takes both the pointer focus and the events away from whatever sits
 * below it, while painting nothing at all.
 *
 * Used directly it is a shield: it makes the area a bloomed window vacated deaf
 * to hover and clicks. ThumbnailOverlay derives from it to also report clicks.
 */
class OverlayWindow : public QRasterWindow
{
    Q_OBJECT

public:
    OverlayWindow();
    ~OverlayWindow() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
};

/*!
 * The click target placed exactly on top of a thumbnail.
 *
 * It covers the thumbnail's rectangle, paints nothing and reports the clicks it
 * swallows through clicked(). The hover outline is drawn by the effect instead,
 * so that it stays in step with the animation.
 */
class ThumbnailOverlay : public OverlayWindow
{
    Q_OBJECT

public:
    ThumbnailOverlay();
    ~ThumbnailOverlay() override;

Q_SIGNALS:
    /*! The overlay, and with it the thumbnail below it, was clicked. */
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
};

} // namespace ThumbnailBloom
