/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailoverlay.h"

#include <QMouseEvent>
#include <QPainter>

namespace ThumbnailBloom
{

ThumbnailOverlay::ThumbnailOverlay()
{
    // Qt::Tool must not be used here: it is Qt::Popup | Qt::Dialog, and KWin
    // turns any internal window carrying Qt::Popup into a grabbing popup
    // (InternalWindow::hasPopupGrab()). PopupInputFilter then swallows every
    // key event and every button press landing outside the overlay, which
    // makes the whole session unclickable.
    setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus | Qt::BypassWindowManagerHint);

    // The window must stay invisible: the thumbnail underneath is what the user
    // sees, so an alpha channel is requested and every pixel is cleared.
    QSurfaceFormat format = requestedFormat();
    format.setAlphaBufferSize(8);
    setFormat(format);
}

ThumbnailOverlay::~ThumbnailOverlay() = default;

void ThumbnailOverlay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(event->rect(), Qt::transparent);
}

void ThumbnailOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    event->accept();
    Q_EMIT clicked();
}

} // namespace ThumbnailBloom
