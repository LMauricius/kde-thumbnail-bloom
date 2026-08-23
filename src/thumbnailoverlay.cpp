/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailoverlay.h"

#include <KColorScheme>

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QStyleHints>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <vector>

namespace ThumbnailBloom {

// ---------------------------------------------------------------------------
// Caption
// ---------------------------------------------------------------------------

//! Fixed metrics of the caption, in logical pixels.
constexpr qreal captionIconSize = 32.0;
constexpr qreal captionIconGap = 1.0;
constexpr qreal captionPaddingX = 5.0;
constexpr qreal captionPaddingY = 1.5;
constexpr qreal captionRadius = 6.0;
constexpr qreal captionMargin = 6.0;
//! How opaque the plate behind the title is.
constexpr qreal captionPlateAlpha = 0.45;
//! How far the shadow reaches, how far it drops, and how dark it is.
constexpr qreal iconShadowRadius = 4.0;
constexpr qreal captionShadowRadius = 1.25;
constexpr qreal captionShadowDrop = 0.3;
constexpr qreal captionShadowAlpha = 0.75;

/*! Returns the colour the caption text is written in. */
static QColor captionTextColor()
{
    // Read on every use: the colour scheme can change while the effect runs.
    return KColorScheme(QPalette::Active, KColorScheme::Window).foreground().color();
}

/*! Returns the colour that reads behind \a textColor: black under a light text,
 * white under a dark one. */
static QColor captionPlateColor(const QColor &textColor)
{
    // Rec. 709 luminance.
    const qreal luminance
        = 0.2126 * textColor.redF() + 0.7152 * textColor.greenF() + 0.0722 * textColor.blueF();
    return luminance > 0.5 ? QColor(Qt::black) : QColor(Qt::white);
}

/*! Returns the box radius the three blur passes of a shadow of \a radius device
 * pixels use. */
static int boxRadius(qreal radius)
{
    // Three box passes of half the radius add up to about a Gaussian of it.
    return std::max(1, qRound(radius / 2.0));
}

/*!
 * Blurs \a image with one box pass of \a radius, along the rows or the columns.
 *
 * A running sum per channel, so the cost does not grow with the radius. Edge
 * pixels are repeated rather than treated as transparent, which is harmless
 * here: every image passed in is padded with enough empty space for the whole
 * kernel, so the repeated pixels are empty ones.
 */
static void boxBlurPass(QImage &image, int radius, bool horizontal)
{
    const int outer = horizontal ? image.height() : image.width();
    const int inner = horizontal ? image.width() : image.height();
    const int window = 2 * radius + 1;
    if (inner <= 0 || outer <= 0) {
        return;
    }

    std::vector<QRgb> line(inner);
    for (int o = 0; o < outer; ++o) {
        // The line is copied out first, because the pass writes over its input.
        for (int i = 0; i < inner; ++i) {
            line[i] = horizontal ? reinterpret_cast<const QRgb *>(image.constScanLine(o))[i]
                                 : reinterpret_cast<const QRgb *>(image.constScanLine(i))[o];
        }

        int sumA = 0, sumR = 0, sumG = 0, sumB = 0;
        for (int i = -radius; i <= radius; ++i) {
            const QRgb pixel = line[std::clamp(i, 0, inner - 1)];
            sumA += qAlpha(pixel);
            sumR += qRed(pixel);
            sumG += qGreen(pixel);
            sumB += qBlue(pixel);
        }

        for (int i = 0; i < inner; ++i) {
            const QRgb blurred = qRgba(sumR / window, sumG / window, sumB / window, sumA / window);
            if (horizontal) {
                reinterpret_cast<QRgb *>(image.scanLine(o))[i] = blurred;
            } else {
                reinterpret_cast<QRgb *>(image.scanLine(i))[o] = blurred;
            }

            const QRgb leaving = line[std::clamp(i - radius, 0, inner - 1)];
            const QRgb entering = line[std::clamp(i + radius + 1, 0, inner - 1)];
            sumA += qAlpha(entering) - qAlpha(leaving);
            sumR += qRed(entering) - qRed(leaving);
            sumG += qGreen(entering) - qGreen(leaving);
            sumB += qBlue(entering) - qBlue(leaving);
        }
    }
}

/*!
 * Draws a blurred shadow of \a draw with \a painter, in \a color, under \a
 * rect.
 *
 * \a draw paints the shape into a buffer of its own, with the origin at the top
 * left of \a rect and at \a dpr device pixels per logical one; whatever it
 * paints is taken for its alpha only. The buffer is padded by the reach of the
 * kernel, blurred by three box passes (a Gaussian is not to be had from
 * QPainter: its blur helpers are private and the widget effects are out of
 * reach), then tinted and stamped a little below the shape.
 */
static void paintShadow(QPainter &painter, const QRectF &rect, qreal dpr, const QColor &color,
    qreal shadowRadius, const std::function<void(QPainter &)> &draw)
{
    if (rect.isEmpty()) {
        return;
    }

    const int box = boxRadius(shadowRadius * dpr);
    const int pad = 3 * box;
    const QSize size((rect.width() * dpr) + 2 * pad, (rect.height() * dpr) + 2 * pad);

    QImage buffer(size, QImage::Format_ARGB32_Premultiplied);
    buffer.fill(Qt::transparent);
    {
        QPainter shapePainter(&buffer);
        shapePainter.setRenderHints(
            QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
        shapePainter.translate(pad, pad);
        shapePainter.scale(dpr, dpr);
        draw(shapePainter);
    }

    for (int pass = 0; pass < 3; ++pass) {
        boxBlurPass(buffer, box, true);
        boxBlurPass(buffer, box, false);
    }

    // SourceIn keeps the blurred alpha and throws the colours away, which is what
    // turns the shape into a shadow of one colour.
    {
        QPainter tintPainter(&buffer);
        tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tintPainter.fillRect(buffer.rect(), color);
    }

    const QRectF target(rect.left() - pad / dpr, rect.top() - pad / dpr + captionShadowDrop,
        size.width() / dpr, size.height() / dpr);
    painter.drawImage(target, buffer);
}

/*!
 * How far past its shape a shadow of \a shadowRadius reaches, in logical pixels.
 *
 * paintShadow() pads its buffer by the whole reach of the kernel and stamps the
 * result a little below the shape, so the ink of a caption can be this far
 * outside the icon and the plate on any side.
 */
static qreal shadowReach(qreal shadowRadius, qreal dpr)
{
    return 3.0 * boxRadius(shadowRadius * dpr) / dpr + captionShadowDrop;
}

/*!
 * Everything drawing a caption needs worked out ahead of the drawing: where the
 * two parts go, what is left of the title, and the strip of the window the
 * result occupies. An empty band means there is nothing to draw at all.
 */
struct CaptionLayout
{
    QRectF iconRect; //!< where the icon goes, empty when there is none
    QRectF plateRect; //!< where the plate behind the title goes, empty when there is none
    QString text; //!< the title, elided to what the plate holds
    QFont font; //!< the font the title was measured with
    QRectF band; //!< everything the caption paints into, the shadows included
};

/*!
 * Lays out a caption of \a icon and \a title along the bottom of \a area.
 *
 * The icon sits to the left of the title, and only the title gets a plate
 * behind it. Nothing is laid out at all when the area is too small to hold the
 * result.
 */
static CaptionLayout captionLayout(
    const QRectF &area, const QIcon &icon, const QString &title, qreal dpr)
{
    CaptionLayout layout;

    const QRectF inner
        = area.adjusted(captionMargin, captionMargin, -captionMargin, -captionMargin);
    if (inner.isEmpty()) {
        return layout;
    }

    layout.font = QGuiApplication::font();
    const QFontMetricsF metrics(layout.font);

    // Both parts are optional, and either one missing simply takes its band out.
    const bool hasIcon = !icon.isNull() && captionIconSize <= inner.width();
    const qreal iconBand = hasIcon ? captionIconSize + captionIconGap : 0.0;

    qreal plateWidth = 0.0;
    qreal plateHeight = 0.0;
    if (!title.isEmpty() && inner.width() - iconBand > 2 * captionPaddingX) {
        const qreal textBudget = inner.width() - iconBand - 2 * captionPaddingX;
        layout.text = metrics.elidedText(title, Qt::ElideRight, textBudget);
        plateWidth = std::min(metrics.horizontalAdvance(layout.text) + 2 * captionPaddingX,
            textBudget + 2 * captionPaddingX);
        plateHeight = metrics.height() + 2 * captionPaddingY;
    }

    // One row, centred on the area and sitting on its bottom edge.
    const qreal width = (hasIcon ? captionIconSize : 0.0)
        + (hasIcon && plateWidth > 0 ? captionIconGap : 0.0) + plateWidth;
    const qreal height = std::max(hasIcon ? captionIconSize : 0.0, plateHeight);
    if (width <= 0 || height <= 0 || height > inner.height() || width > inner.width()) {
        return CaptionLayout();
    }

    const qreal left = inner.center().x() - width / 2;
    const qreal middle = inner.bottom() - height / 2;

    // The band is the union of the two parts, each grown by the reach of its own
    // shadow. It is built part by part rather than with QRectF::united(), which
    // would drag the origin in for a caption that has only one of them.
    if (hasIcon) {
        layout.iconRect
            = QRectF(left, middle - captionIconSize / 2, captionIconSize, captionIconSize);
        const qreal reach = shadowReach(iconShadowRadius, dpr);
        layout.band = layout.iconRect.adjusted(-reach, -reach, reach, reach);
    }
    if (plateHeight > 0) {
        layout.plateRect = QRectF(
            left + (hasIcon ? iconBand : 0.0), middle - plateHeight / 2, plateWidth, plateHeight);
        const qreal reach = shadowReach(captionShadowRadius, dpr);
        const QRectF plateBand = layout.plateRect.adjusted(-reach, -reach, reach, reach);
        layout.band = layout.band.isEmpty() ? plateBand : layout.band.united(plateBand);
    }

    return layout;
}

/*!
 * Draws the caption \a layout describes with \a painter, at \a dpr device
 * pixels per logical one.
 *
 * The plate is black or white depending on which one the text colour can be
 * read against. Both parts carry a shadow: the title one in the colour of its
 * own plate, the icon always a black one, since an icon brings its own colours
 * and has to stay legible over a light background whatever the colour scheme is.
 */
static void paintCaption(
    QPainter &painter, const CaptionLayout &layout, const QIcon &icon, qreal dpr)
{
    const QColor textColor = captionTextColor();
    const QColor plateColor = captionPlateColor(textColor);

    if (!layout.iconRect.isEmpty()) {
        // An icon brings its own colours and has to stay legible over a light
        // background whatever the colour scheme is, so its shadow is black.
        QColor shadowColor(Qt::black);
        shadowColor.setAlphaF(captionShadowAlpha);
        paintShadow(painter, layout.iconRect, dpr, shadowColor, iconShadowRadius,
            [&icon](QPainter &shapePainter) {
                icon.paint(&shapePainter, QRect(0, 0, int(captionIconSize), int(captionIconSize)));
            });

        icon.paint(&painter, layout.iconRect.toRect());
    }

    if (!layout.plateRect.isEmpty()) {
        const QRectF plate = layout.plateRect;

        QColor filled = plateColor;
        filled.setAlphaF(captionPlateAlpha);
        painter.setPen(Qt::NoPen);
        painter.setBrush(filled);
        painter.drawRoundedRect(plate, captionRadius, captionRadius);

        // The shadow of the text is the colour of its own plate, so it deepens
        // the plate under the glyphs instead of colouring them.
        QColor shadowColor = plateColor;
        shadowColor.setAlphaF(captionShadowAlpha);
        const QRectF local(0, 0, plate.width(), plate.height());
        paintShadow(painter, plate, dpr, shadowColor, captionShadowRadius,
            [&layout, local](QPainter &shapePainter) {
                shapePainter.setFont(layout.font);
                shapePainter.drawText(local, Qt::AlignCenter, layout.text);
            });

        painter.setPen(textColor);
        painter.setFont(layout.font);
        painter.drawText(plate, Qt::AlignCenter, layout.text);
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
    setFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus
        | Qt::BypassWindowManagerHint);

    // The window must stay invisible: whatever is underneath is what the user
    // sees, so an alpha channel is requested and every pixel is cleared.
    QSurfaceFormat format = requestedFormat();
    format.setAlphaBufferSize(8);
    setFormat(format);
}

OverlayWindow::~OverlayWindow() = default;

void OverlayWindow::setOutputOnly(bool outputOnly) { setProperty("outputOnly", outputOnly); }

bool OverlayWindow::isOutputOnly() const { return property("outputOnly").toBool(); }

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

void OverlayWindow::mouseMoveEvent(QMouseEvent *event) { event->accept(); }

void OverlayWindow::mousePressEvent(QMouseEvent *event) { event->accept(); }

void OverlayWindow::mouseReleaseEvent(QMouseEvent *event) { event->accept(); }

void OverlayWindow::wheelEvent(QWheelEvent *event) { event->accept(); }

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

    // The rendered caption holds the colours of the scheme and the size of the
    // pixels it was made at, so both have to drop it.
    connect(this, &QWindow::screenChanged, this, [this](QScreen *) { invalidateCaption(); });
    qApp->installEventFilter(this);
}

ThumbnailOverlay::~ThumbnailOverlay() = default;

void ThumbnailOverlay::setCaption(const QIcon &icon, const QString &title)
{
    if (m_title == title && m_icon.cacheKey() == icon.cacheKey()) {
        return;
    }

    m_icon = icon;
    m_title = title;
    invalidateCaption();
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

    // Only the strip the caption is drawn in has to be repainted, which is what
    // keeps a fade from redrawing and re-uploading the whole thumbnail sized
    // window every frame. A caption that has not been laid out yet may land
    // anywhere, so that one still takes the window as a whole; one that will
    // draw nothing at all needs no repaint whatsoever.
    if (m_captionDirty) {
        if (!m_icon.isNull() || !m_title.isEmpty()) {
            update();
        }
    } else if (!m_captionBand.isEmpty()) {
        update(m_captionBand.toAlignedRect());
    }
}

void ThumbnailOverlay::invalidateCaption()
{
    m_captionDirty = true;

    // The band the old caption occupied has to be cleared even when the new one
    // turns out to be smaller, so this repaints everything.
    if (m_captionOpacity > 0) {
        update();
    }
}

void ThumbnailOverlay::renderCaption()
{
    if (!m_captionDirty) {
        return;
    }

    m_captionDirty = false;
    m_captionImage = QImage();
    m_captionBand = QRectF();

    if (m_icon.isNull() && m_title.isEmpty()) {
        return;
    }

    const qreal dpr = devicePixelRatio();
    const CaptionLayout layout = captionLayout(QRectF(QPointF(0, 0), size()), m_icon, m_title, dpr);
    if (layout.band.isEmpty()) {
        return;
    }

    // Blurring the two shadows by hand costs far too much to repeat on every
    // frame of a fade, so the caption is drawn once into an image of its own and
    // only stamped from there on. The image covers the whole band, the shadows
    // included; the fade is then a matter of the opacity it is stamped with.
    //
    // The band is squared up on the grid of the device rather than on the one of
    // the logical pixels, and the image is placed back on it by that same grid:
    // on a screen scaled by anything but a whole number the two do not line up,
    // and stamping the image at a fraction of a device pixel would have QPainter
    // resample it, which would leave the caption softer than it was drawn.
    const QRect device
        = QRectF(layout.band.topLeft() * dpr, layout.band.size() * dpr).toAlignedRect();
    m_captionBand = QRectF(QPointF(device.x() / dpr, device.y() / dpr),
        QSizeF(device.width() / dpr, device.height() / dpr));

    m_captionImage = QImage(device.size(), QImage::Format_ARGB32_Premultiplied);
    m_captionImage.setDevicePixelRatio(dpr);
    m_captionImage.fill(Qt::transparent);

    QPainter painter(&m_captionImage);
    painter.setRenderHints(
        QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    painter.translate(-m_captionBand.topLeft());
    paintCaption(painter, layout, m_icon, dpr);
}

void ThumbnailOverlay::resizeEvent(QResizeEvent *event)
{
    // The caption is laid out against the size of the window, and a resize
    // repaints the whole of it anyway.
    invalidateCaption();
    OverlayWindow::resizeEvent(event);
}

bool ThumbnailOverlay::eventFilter(QObject *watched, QEvent *event)
{
    // In Qt 6 the palette change reaches the application object and nothing
    // else, so that is where a colour scheme change has to be picked up from.
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        invalidateCaption();
    }

    return OverlayWindow::eventFilter(watched, event);
}

void ThumbnailOverlay::paintEvent(QPaintEvent *event)
{
    renderCaption();

    // Source mode clears instead of blending: the backing store keeps what was
    // drawn last time, and blending the new caption over the old one would
    // double it. Only what the repaint asked for is cleared, which is what lets
    // a fade repaint the caption band alone; nothing is ever drawn outside that
    // band, so the rest of the window still holds the pixels a full paint
    // cleared, and invalidateCaption() repaints everything whenever the band
    // itself can have moved.
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect &rect : event->region()) {
        painter.fillRect(rect, Qt::transparent);
    }

    if (m_captionOpacity <= 0 || m_captionImage.isNull()) {
        return;
    }

    // All that is left of the drawing: the caption was rendered once, shadows
    // and all, and the fade is the opacity it is stamped at.
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setClipRegion(event->region());
    painter.setOpacity(std::min(m_captionOpacity, 1.0));
    painter.drawImage(m_captionBand.topLeft(), m_captionImage);
}

bool ThumbnailOverlay::isDrag(const QPointF &origin, const QPointF &pos)
{
    return (pos - origin).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance();
}

void ThumbnailOverlay::resetTouch()
{
    m_touchArmed = false;
    m_longPressTimer.stop();
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
        break;
    }
    case QEvent::TouchUpdate:
        for (const QEventPoint &point : static_cast<QTouchEvent *>(event)->points()) {
            if (!m_touchArmed || point.id() != m_touchId
                || !isDrag(m_touchOrigin, point.globalPosition())) {
                continue;
            }
            // A finger that moved is neither a tap nor a hold any more. From
            // here on the sequence is driven by the effect's own touch filter,
            // because KWin's move filter only follows a point it saw go down.
            resetTouch();
            Q_EMIT dragStarted(point.globalPosition(), m_touchId);
            break;
        }
        break;
    case QEvent::TouchEnd:
        if (m_touchArmed) {
            resetTouch();
            Q_EMIT activated();
        }
        break;
    case QEvent::TouchCancel:
        resetTouch();
        break;
    default:
        return OverlayWindow::event(event);
    }

    // The whole sequence is accepted whatever came of it, so Qt neither hands
    // it on nor synthesises a mouse event out of it for the window below.
    event->accept();
    return true;
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
