/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailoverlay.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleHints>
#include <QTouchEvent>
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

ThumbnailOverlay::ThumbnailOverlay()
{
    // A touch has no second button, so holding still stands in for a right
    // click. The interval is the one the rest of the desktop presses and holds
    // for, so the gesture feels the same everywhere.
    m_longPressTimer.setSingleShot(true);
    m_longPressTimer.setInterval(QGuiApplication::styleHints()->mousePressAndHoldInterval());
    connect(&m_longPressTimer, &QTimer::timeout, this, [this]() {
        if (m_touchArmed) {
            m_touchArmed = false;
            Q_EMIT menuRequested(m_touchOrigin);
        }
    });
}

ThumbnailOverlay::~ThumbnailOverlay() = default;

bool ThumbnailOverlay::isDrag(const QPointF &origin, const QPointF &pos)
{
    return (pos - origin).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance();
}

bool ThumbnailOverlay::event(QEvent *event)
{
    // Only the first point of a sequence is followed; the others belong to
    // whatever gesture is running elsewhere and are swallowed by the base class.
    switch (event->type()) {
    case QEvent::TouchBegin: {
        const QList<QEventPoint> &points = static_cast<QTouchEvent *>(event)->points();
        if (!m_touchArmed && !points.isEmpty()) {
            m_touchId = points.first().id();
            m_touchOrigin = points.first().globalPosition();
            m_touchArmed = true;
            m_longPressTimer.start();
        }
        event->accept();
        return true;
    }
    case QEvent::TouchUpdate: {
        for (const QEventPoint &point : static_cast<QTouchEvent *>(event)->points()) {
            if (!m_touchArmed || point.id() != m_touchId || !isDrag(m_touchOrigin, point.globalPosition())) {
                continue;
            }
            // A finger that moved is neither a tap nor a hold any more. From
            // here on the sequence is driven by the effect's own touch filter,
            // because KWin's move filter only follows a point it saw go down.
            m_touchArmed = false;
            m_longPressTimer.stop();
            event->accept();
            Q_EMIT dragStarted(point.globalPosition(), m_touchId);
            return true;
        }
        event->accept();
        return true;
    }
    case QEvent::TouchEnd: {
        if (m_touchArmed) {
            m_touchArmed = false;
            m_longPressTimer.stop();
            event->accept();
            Q_EMIT activated();
            return true;
        }
        event->accept();
        return true;
    }
    case QEvent::TouchCancel:
        m_touchArmed = false;
        m_longPressTimer.stop();
        event->accept();
        return true;
    default:
        return OverlayWindow::event(event);
    }
}

// Every button is still swallowed, they just do not all mean something.

void ThumbnailOverlay::mousePressEvent(QMouseEvent *event)
{
    event->accept();

    // The left button decides nothing yet: what happens next (a release or a
    // move) is what tells an activation from a drag apart.
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        m_pressOrigin = event->globalPosition();
    } else if (event->button() == Qt::RightButton) {
        Q_EMIT menuRequested(event->globalPosition());
    }
}

void ThumbnailOverlay::mouseMoveEvent(QMouseEvent *event)
{
    event->accept();
    if (!m_pressed || !isDrag(m_pressOrigin, event->globalPosition())) {
        return;
    }

    // The current position, not the one the press started at: the window is
    // grabbed where the pointer is, so it does not jump by the drag threshold.
    m_pressed = false;
    Q_EMIT dragStarted(event->globalPosition(), -1);
}

void ThumbnailOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
    if (event->button() == Qt::LeftButton && m_pressed) {
        m_pressed = false;
        Q_EMIT activated();
    }
}

} // namespace ThumbnailBloom
