/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailoverlay.h"

#include <KColorScheme>

#include <QMouseEvent>
#include <QPainter>

namespace ThumbnailBloom
{

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

namespace
{
//! Width of the hover outline, in logical pixels.
constexpr qreal outlineWidth = 2.0;
//! Corner radius of the hover outline, in logical pixels.
constexpr qreal outlineRadius = 4.0;

/*! Returns the outline colour of the current colour scheme. */
QColor outlineColor()
{
    // Read on every paint: the colour scheme can change while the effect runs.
    return KColorScheme(QPalette::Active, KColorScheme::View).decoration(KColorScheme::FocusColor).color();
}
}

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

void ThumbnailOverlay::setHovered(bool hovered)
{
    if (m_hovered == hovered) {
        return;
    }

    m_hovered = hovered;
    update();
}

bool ThumbnailOverlay::isHovered() const
{
    return m_hovered;
}


void ThumbnailOverlay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    // Source mode clears instead of blending, so the thumbnail below stays visible.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(event->rect(), Qt::transparent);

    if (!m_hovered) {
        return;
    }

    // The overlay sits in the popup layer, above the windows and so above the
    // lifted thumbnail as well: the outline is simply painted over the edge of
    // the thumbnail, inset by half the pen width to keep the whole stroke inside.
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(outlineColor(), outlineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0, 0, width(), height()).adjusted(outlineWidth / 2, outlineWidth / 2, -outlineWidth / 2, -outlineWidth / 2),
                            outlineRadius, outlineRadius);
}

void ThumbnailOverlay::mouseMoveEvent(QMouseEvent *event)
{
    // Accepting is what keeps the motion from reaching the window below: KWin
    // hands the event to the internal window first and only passes it on to the
    // client under the pointer if it comes back ignored. Without this the window
    // that happens to sit behind the thumbnail lights up its own hover feedback.
    event->accept();
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
