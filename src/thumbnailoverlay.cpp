/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailoverlay.h"

#include <KColorScheme>

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleHints>
#include <QTouchEvent>
#include <QWheelEvent>

namespace ThumbnailBloom
{

// ---------------------------------------------------------------------------
// Caption
// ---------------------------------------------------------------------------

//! Fixed metrics of the caption, in logical pixels.
constexpr qreal captionIconSize = 32.0;
constexpr qreal captionIconGap = 4.0;
constexpr qreal captionPaddingX = 8.0;
constexpr qreal captionPaddingY = 3.0;
constexpr qreal captionRadius = 6.0;
constexpr qreal captionMargin = 6.0;
//! How opaque the plate behind the title is.
constexpr qreal captionPlateAlpha = 0.55;

/*! Returns the colour the caption text is written in. */
static QColor captionTextColor()
{
    // Read on every use: the colour scheme can change while the effect runs.
    return KColorScheme(QPalette::Active, KColorScheme::Window).foreground().color();
}

/*!
 * Draws \a icon above \a title with \a painter, at the bottom of \a area.
 *
 * The icon sits free, and only the title gets a plate behind it, black or white
 * depending on which one the text colour can be read against. Nothing is drawn
 * at all when the area is too small to hold the result.
 */
static void paintCaption(QPainter &painter, const QRectF &area, const QIcon &icon, const QString &title)
{
    const QRectF inner = area.adjusted(captionMargin, captionMargin, -captionMargin, -captionMargin);
    if (inner.isEmpty()) {
        return;
    }

    const QFont font = QGuiApplication::font();
    const QFontMetricsF metrics(font);

    // Both parts are optional, and either one missing simply takes its band out.
    const bool hasIcon = !icon.isNull();
    const qreal iconHeight = hasIcon ? captionIconSize : 0.0;

    QString text;
    qreal plateWidth = 0.0;
    qreal plateHeight = 0.0;
    if (!title.isEmpty() && inner.width() > 2 * captionPaddingX) {
        text = metrics.elidedText(title, Qt::ElideRight, inner.width() - 2 * captionPaddingX);
        plateWidth = std::min(metrics.horizontalAdvance(text) + 2 * captionPaddingX, inner.width());
        plateHeight = metrics.height() + 2 * captionPaddingY;
    }

    const qreal height = iconHeight + (hasIcon && plateHeight > 0 ? captionIconGap : 0.0) + plateHeight;
    if (height <= 0 || height > inner.height() || (hasIcon && captionIconSize > inner.width())) {
        return;
    }

    if (hasIcon) {
        const QRectF iconRect(inner.center().x() - captionIconSize / 2,
                              inner.bottom() - height,
                              captionIconSize, captionIconSize);
        icon.paint(&painter, iconRect.toRect());
    }

    if (plateHeight > 0) {
        const QColor textColor = captionTextColor();
        // Rec. 709 luminance: a light text needs a dark plate and the other way round.
        const qreal luminance = 0.2126 * textColor.redF() + 0.7152 * textColor.greenF() + 0.0722 * textColor.blueF();
        QColor plateColor = luminance > 0.5 ? QColor(Qt::black) : QColor(Qt::white);
        plateColor.setAlphaF(captionPlateAlpha);

        const QRectF plate(inner.center().x() - plateWidth / 2, inner.bottom() - plateHeight, plateWidth, plateHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(plateColor);
        painter.drawRoundedRect(plate, captionRadius, captionRadius);

        painter.setPen(textColor);
        painter.setFont(font);
        painter.drawText(plate, Qt::AlignCenter, text);
    }
}

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

void OverlayWindow::setOutputOnly(bool outputOnly)
{
    setProperty("outputOnly", outputOnly);
}

bool OverlayWindow::isOutputOnly() const
{
    return property("outputOnly").toBool();
}

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
        if (m_touchArmed && !isOutputOnly()) {
            m_touchArmed = false;
            Q_EMIT menuRequested(m_touchOrigin);
        }
    });
}

ThumbnailOverlay::~ThumbnailOverlay() = default;

void ThumbnailOverlay::setCaption(const QIcon &icon, const QString &title)
{
    if (m_title == title && m_icon.cacheKey() == icon.cacheKey()) {
        return;
    }

    m_icon = icon;
    m_title = title;
    if (m_captionOpacity > 0) {
        update();
    }
}

void ThumbnailOverlay::setCaptionOpacity(qreal opacity)
{
    // Repainted only when the change can be seen: the fade runs on the
    // compositor's clock and would otherwise queue a repaint every frame for
    // steps far below one step of alpha.
    if (std::abs(opacity - m_captionOpacity) < 1.0 / 255.0) {
        return;
    }

    m_captionOpacity = opacity;
    update();
}

void ThumbnailOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    // The whole window is cleared, not just the exposed part: the backing store
    // keeps what was drawn last time, so clearing less would leave the previous
    // caption underneath the new one and blend the two together.
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(QRect(QPoint(0, 0), size()), Qt::transparent);

    if (m_captionOpacity <= 0 || (m_icon.isNull() && m_title.isEmpty())) {
        return;
    }

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    painter.setOpacity(std::min(m_captionOpacity, 1.0));
    paintCaption(painter, QRectF(QPointF(0, 0), size()), m_icon, m_title);
}

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
        if (!m_touchArmed && !points.isEmpty() && !isOutputOnly()) {
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

    if (isOutputOnly()) {
        return;
    }

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
