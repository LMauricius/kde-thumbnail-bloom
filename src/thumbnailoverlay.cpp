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
    // The window type is load bearing, because KWin derives the behaviour of an
    // internal window from the Qt flags:
    //  - Qt::Tool is Qt::Popup | Qt::Dialog, and anything carrying Qt::Popup
    //    becomes a grabbing popup (InternalWindow::hasPopupGrab()), after which
    //    PopupInputFilter swallows every key event and every button press that
    //    lands outside the overlay. The session becomes unusable.
    //  - A plain Qt::Window makes the overlay a normal window: it then shows up
    //    in the window list of every KWin script and in anything that walks the
    //    stacking order looking for real windows.
    // Qt::ToolTip is Qt::Popup | Qt::Sheet, and hasPopupGrab() excludes tooltips
    // explicitly, so it gives input without a grab and stays out of the way.
    setFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::BypassWindowManagerHint);

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
