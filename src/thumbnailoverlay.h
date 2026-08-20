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
 * target of a thumbnail: it covers the thumbnail's rectangle, paints nothing
 * but the hover outline and reports the pointer events it swallows through
 * clicked() and hoverChanged().
 */
class ThumbnailOverlay : public QRasterWindow
{
    Q_OBJECT

public:
    ThumbnailOverlay();
    ~ThumbnailOverlay() override;

    /*!
     * Sets whether the thumbnail below is hovered, and with it whether the
     * outline is painted.
     *
     * The overlay cannot work this out on its own: KWin dispatches pointer
     * events to its internal windows itself and never synthesises the enter and
     * leave events a QWindow would normally get, so the effect tracks the cursor
     * and tells the overlay.
     */
    void setHovered(bool hovered);
    /*! Whether the thumbnail below is currently marked as hovered. */
    bool isHovered() const;

Q_SIGNALS:
    /*! The overlay, and with it the thumbnail below it, was clicked. */
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    bool m_hovered = false;
};

} // namespace ThumbnailBloom
