/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailoverlay.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

namespace ThumbnailBloom
{

OverlayWindow::OverlayWindow()
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

    // The window must stay invisible: whatever is underneath is what the user
    // sees, so an alpha channel is requested and every pixel is cleared.
    QSurfaceFormat format = requestedFormat();
    format.setAlphaBufferSize(8);
    setFormat(format);
}

OverlayWindow::~OverlayWindow() = default;

bool OverlayWindow::event(QEvent *event)
{
    // Touch has no per button handler to override, and an unhandled touch event
    // is turned into a synthetic mouse event by Qt, which would then reach the
    // window below. Accepting the whole sequence here stops both.
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        event->accept();
        return true;
    default:
        return QRasterWindow::event(event);
    }
}

void OverlayWindow::paintEvent(QPaintEvent *event)
{
    // Source mode clears instead of blending, so what is below stays visible.
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(event->rect(), Qt::transparent);
}

// Accepting is what keeps an event from reaching the window below: KWin hands it
// to the internal window first and only passes it on to the client under the
// pointer if it comes back ignored. Without this the window that happens to sit
// behind lights up its own hover feedback, scrolls, or gets raised.

void OverlayWindow::mouseMoveEvent(QMouseEvent *event)
{
    event->accept();
}

void OverlayWindow::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void OverlayWindow::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void OverlayWindow::wheelEvent(QWheelEvent *event)
{
    event->accept();
}

ThumbnailOverlay::ThumbnailOverlay() = default;

ThumbnailOverlay::~ThumbnailOverlay() = default;

bool ThumbnailOverlay::event(QEvent *event)
{
    // A tap does what a left click does. The rest of the sequence is swallowed
    // by the base class, so a drag started on a thumbnail activates it once and
    // then goes nowhere.
    if (event->type() == QEvent::TouchBegin) {
        event->accept();
        Q_EMIT clicked();
        return true;
    }

    return OverlayWindow::event(event);
}

void ThumbnailOverlay::mousePressEvent(QMouseEvent *event)
{
    // Other buttons are still swallowed, they just do not activate anything.
    event->accept();
    if (event->button() == Qt::LeftButton) {
        Q_EMIT clicked();
    }
}

} // namespace ThumbnailBloom
