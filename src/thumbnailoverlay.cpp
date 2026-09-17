/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailoverlay.h"

#include <KColorScheme>

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QScreen>
#include <QStyleHints>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace ThumbnailBloom {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/*!
 * Returns \a point moved onto the grid of whole physical pixels, there being
 * \a dpr of them to a logical one.
 */
static QPointF snapToDevice(const QPointF &point, qreal dpr)
{
    return QPointF(std::round(point.x() * dpr) / dpr, std::round(point.y() * dpr) / dpr);
}

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

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

/*!
 * Returns the part of the store a frame drawn \a width wide around \a corners
 * covers, rounded out to whole pixels.
 *
 * The whole of the frame lies outside the corners it is given, and the coverage
 * ramp that antialiases it reaches a pixel further still.
 */
static QRect outlineBounds(const std::array<QPointF, 4> &corners, qreal width)
{
    if (width <= 0.0) {
        return QRect();
    }

    const QRectF bounds
        = QPolygonF({ corners[0], corners[1], corners[2], corners[3] }).boundingRect();
    const qreal reach = width + 1.0;
    return bounds.adjusted(-reach, -reach, reach, reach).toAlignedRect();
}

/*!
 * Returns the part of the store a frame drawn \a width wide around \a corners
 * really covers: the band around the picture, with the picture itself taken out
 * of it.
 *
 * The whole of the frame lies outside the corners it is given, so no ink ever
 * reaches the inside of the quad they describe and the largest rectangle that
 * fits in there can be cut out. That hole is the point of measuring a region
 * rather than the rectangle around the band: the rectangle around the band is
 * the whole thumbnail, while the band itself is a line along the edge of it, and
 * everything the store costs per frame is measured by the pixel.
 *
 * The rectangle is pulled in by a pixel on every side first, which is as far as
 * the coverage ramp that antialiases the fill can reach past the edge it is
 * drawn along, and then rounded inwards to whole pixels, so that a pixel the
 * fill so much as laps onto is never taken for one it left alone.
 */
static QRegion outlineRegion(const std::array<QPointF, 4> &corners, qreal width)
{
    const QRect bounds = outlineBounds(corners, width);
    if (bounds.isEmpty()) {
        return QRegion();
    }

    // The corners run clockwise from the top left, so the rectangle inside the
    // quad is bounded by the inner one of each opposing pair, whatever the bend
    // has done to them.
    const qreal left = std::max(corners[0].x(), corners[3].x()) + 1.0;
    const qreal right = std::min(corners[1].x(), corners[2].x()) - 1.0;
    const qreal top = std::max(corners[0].y(), corners[1].y()) + 1.0;
    const qreal bottom = std::min(corners[2].y(), corners[3].y()) - 1.0;

    // A pixel covers the half-open square from its own coordinate to the next
    // one, so the last one that lies wholly inside ends a pixel short of the
    // boundary; QRect takes both corners inclusively.
    const QRect hole(QPoint(std::ceil(left), std::ceil(top)),
        QPoint(std::floor(right) - 1, std::floor(bottom) - 1));
    if (hole.isEmpty()) {
        return QRegion(bounds);
    }

    return QRegion(bounds) - hole;
}

/*!
 * Returns the ring to fill for a frame \a width wide lying outside \a corners,
 * which run clockwise from the top left.
 *
 * Sharp on the inside, where it meets the picture, and rounded on the outside by
 * the width of the frame itself. Each outer corner is an arc centred on the
 * inner corner, which is what makes that radius the right one: every point of
 * the arc is then exactly \a width from the corner the frame is drawn around, so
 * the ring is the same thickness at the corners as it is along the edges. It is
 * a fill rather than a stroke because a pen rounds a join by half its width and
 * rounds the inside of it too.
 */
static QPainterPath outlinePath(const std::array<QPointF, 4> &corners, qreal width)
{
    //! Outward normal of the edge leaving corner \a i, the corners running clockwise.
    const auto normal = [&corners](int i) {
        const QPointF edge = corners[(i + 1) % 4] - corners[i];
        const qreal length = std::hypot(edge.x(), edge.y());
        return length > 0.0 ? QPointF(edge.y() / length, -edge.x() / length) : QPointF();
    };
    //! The same direction as the angle QPainterPath measures arcs in.
    const auto angle = [](const QPointF &direction) {
        return std::atan2(-direction.y(), direction.x()) * 180.0 / M_PI;
    };

    QPainterPath path;
    for (int i = 0; i < 4; ++i) {
        const QRectF arc(corners[i] - QPointF(width, width), QSizeF(2.0 * width, 2.0 * width));
        const qreal from = angle(normal((i + 3) % 4));
        qreal sweep = angle(normal(i)) - from;

        // Clockwise, the way the corners themselves run, so that the arc is the
        // short way round the outside of the corner rather than the long way
        // round the inside of it.
        while (sweep > 0.0) {
            sweep -= 360.0;
        }
        while (sweep <= -360.0) {
            sweep += 360.0;
        }

        if (i == 0) {
            path.arcMoveTo(arc, from);
        }
        path.arcTo(arc, from, sweep);
    }
    path.closeSubpath();

    // The hole. Odd-even fill is what makes the second subpath one: the picture
    // reaches right into the corner, and only the outside of the frame is
    // rounded.
    path.addPolygon(QPolygonF({ corners[0], corners[1], corners[2], corners[3] }));
    path.closeSubpath();
    path.setFillRule(Qt::OddEvenFill);
    return path;
}

ThumbnailCanvas::ThumbnailCanvas()
{
    // It paints and nothing else: the click target underneath answers for the
    // whole thumbnail, and two windows fighting over the same pixel would give
    // the pointer nowhere to settle. KWin reads this property in
    // InternalWindow::hitTest(), which is the only way an internal window can
    // stay on screen and yet be missed by the hit test; hiding it would take the
    // painting with it.
    setProperty("outputOnly", true);

    // Hiding an internal window destroys it, and what comes back may be holding
    // a buffer of its own, so the first paint after a window has been away
    // clears the whole store rather than only the ground of what it drew last.
    connect(this, &QWindow::visibleChanged, this, [this](bool) {
        m_paintedSize = QSize();
        m_painted = QRegion();
    });

    // The rendered caption holds the colours of the scheme and the size of the
    // pixels it was made at, so both have to drop it.
    connect(this, &QWindow::screenChanged, this, [this](QScreen *) { invalidateCaption(); });
}

QRectF ThumbnailCanvas::shownRect() const { return m_shown; }

void ThumbnailCanvas::setCaption(const QIcon &icon, const QString &title, const QSizeF &restSize)
{
    if (m_title == title && m_icon.cacheKey() == icon.cacheKey() && restSize == m_restSize) {
        return;
    }

    m_icon = icon;
    m_title = title;
    m_restSize = restSize;
    invalidateCaption();
}

QRectF ThumbnailCanvas::captionRect() const
{
    if (m_captionImage.isNull() || m_captionOpacity <= 0.0 || m_content.isEmpty()) {
        return QRectF();
    }

    // The caption hangs from the bottom centre of the picture wherever that is
    // drawn at this moment, the band having been measured from the same point of
    // the resting rectangle. So it is laid out once and rides the picture through
    // a trip, rather than being laid out again for every size the picture passes
    // through on the way; at rest the two rectangles are the same one, which is
    // where the caption has to be exactly right.
    //
    // Snapping puts it back on the grid the image was made on. The band was
    // squared up on the one of the device when it was rendered, and stamping it
    // at a fraction of a device pixel would have QPainter resample it, which
    // would leave the caption softer than it was drawn.
    const QPointF anchor(m_content.center().x(), m_content.bottom());
    return QRectF(
        snapToDevice(anchor + m_captionBand.topLeft(), devicePixelRatio()), m_captionBand.size());
}

QRegion ThumbnailCanvas::paintedRegion() const { return m_painted; }

QRegion ThumbnailCanvas::drawnBounds() const
{
    QRegion drawn = outlineRegion(m_corners, m_width);

    // Clipped the way the paint clips it, so that what is cleared and what is
    // drawn are the same ground.
    const QRectF caption = captionRect().intersected(m_content);
    if (!caption.isEmpty()) {
        // Rounded outwards: a band lying between two pixels covers both of them,
        // and the one it only laps onto still has to be cleared and drawn again.
        drawn += caption.toAlignedRect();
    }

    return drawn;
}

void ThumbnailCanvas::setContent(const QRectF &content, const std::array<QPointF, 4> &corners,
    qreal width, const QColor &color, qreal strength, qreal captionOpacity)
{
    // A twentieth of a pixel either way is not worth a repaint and the upload
    // that comes with it, and neither is a step of alpha below one in 255: every
    // frame of a hover offers a slightly different line and a slightly further
    // fade, and the difference between two of them cannot be seen. A caption
    // waiting to be drawn again always counts, since nothing else need have
    // moved for the title or the colour scheme to have changed.
    const auto same = [](qreal a, qreal b) { return std::abs(a - b) < 0.05; };
    bool changed = m_captionDirty || !same(width, m_width) || !same(strength, m_strength)
        || color != m_color || std::abs(captionOpacity - m_captionOpacity) >= 1.0 / 255.0
        || !same(content.x(), m_content.x()) || !same(content.y(), m_content.y())
        || !same(content.width(), m_content.width()) || !same(content.height(), m_content.height());
    for (size_t i = 0; i < corners.size() && !changed; ++i) {
        changed
            = !same(corners[i].x(), m_corners[i].x()) || !same(corners[i].y(), m_corners[i].y());
    }
    if (!changed) {
        return;
    }

    // What the buffer holds at this moment. The repaint has to cover it as well
    // as the ground of the new drawing, the one being cleared and the other put
    // in its place.
    const QRegion stale = m_painted;

    m_content = content;
    m_corners = corners;
    m_width = width;
    m_color = color;
    m_strength = strength;
    m_captionOpacity = captionOpacity;

    // Rendered here rather than in the paint, so that the band it lands on is
    // known before the repaint is asked for and the repaint can be asked for
    // over that band alone. A store is as large as the largest rectangle the
    // running trip draws, which for a thumbnail on its way home is the whole
    // window, and asking for all of it would have Qt upload all of it on every
    // frame of the trip.
    renderCaption();

    update(stale.united(drawnBounds()));

    // Painted in this very turn rather than whenever the event loop gets round
    // to it. The effect hands this over from inside the pass that is drawing the
    // step of the animation it belongs to, and it has already moved the store
    // onto the thumbnail; a paint left to the update timer would put the step
    // before into the buffer the pass then draws, which is a frame of lag against
    // a thumbnail in motion. Sending the request by hand is what Qt's own timer
    // does when it goes off, so the paint and the flush that follow it are the
    // ordinary ones and only the moment is ours. An unexposed window has nothing
    // to paint into and is left to the timer.
    if (isExposed()) {
        QEvent request(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(this, &request);
    }
}

void ThumbnailCanvas::invalidateCaption()
{
    m_captionDirty = true;

    // The band the old caption occupied has to be cleared even when the new one
    // turns out smaller or lands elsewhere, so this one asks for the whole store.
    // It is the rare path: a title, a colour scheme or a screen changing, and the
    // resting size of the thumbnail. Every frame of a fade or of a trip goes
    // through setContent(), which pays band by band.
    if (m_captionOpacity > 0.0) {
        update();
    }
}

void ThumbnailCanvas::renderCaption()
{
    if (!m_captionDirty) {
        return;
    }

    m_captionDirty = false;
    m_captionImage = QImage();
    m_captionBand = QRectF();

    if ((m_icon.isNull() && m_title.isEmpty()) || m_restSize.isEmpty()) {
        return;
    }

    const qreal dpr = devicePixelRatio();
    // Laid out against a rectangle at the origin rather than against the window:
    // the area the caption sits in travels with the thumbnail, and keeping the
    // band relative to it is what lets the image outlive the trip.
    const CaptionLayout layout
        = captionLayout(QRectF(QPointF(0, 0), m_restSize), m_icon, m_title, dpr);
    if (layout.band.isEmpty()) {
        return;
    }

    // Blurring the two shadows by hand costs far too much to repeat on every
    // frame of a fade, let alone of a trip, so the caption is drawn once into an
    // image of its own and only stamped from there on. The image covers the whole
    // band, the shadows included; the fade is then a matter of the opacity it is
    // stamped with.
    //
    // The band is squared up on the grid of the device rather than on the one of
    // the logical pixels: on a screen scaled by anything but a whole number the
    // two do not line up, and an image sized to a fraction of a device pixel
    // would be resampled wherever it was put. captionRect() lands it back on that
    // same grid.
    const QRect device
        = QRectF(layout.band.topLeft() * dpr, layout.band.size() * dpr).toAlignedRect();
    const QRectF band(QPointF(device.x() / dpr, device.y() / dpr),
        QSizeF(device.width() / dpr, device.height() / dpr));

    m_captionImage = QImage(device.size(), QImage::Format_ARGB32_Premultiplied);
    m_captionImage.setDevicePixelRatio(dpr);
    m_captionImage.fill(Qt::transparent);

    QPainter painter(&m_captionImage);
    painter.setRenderHints(
        QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    painter.translate(-band.topLeft());
    paintCaption(painter, layout, m_icon, dpr);

    // Kept as an offset from the bottom centre of the rectangle it was laid out
    // for, which is the point captionRect() hangs it from on the picture. So the
    // image outlives every trip: only the anchor moves.
    m_captionBand = band.translated(-QPointF(m_restSize.width() / 2, m_restSize.height()));
}

void ThumbnailCanvas::paintEvent(QPaintEvent *event)
{
    renderCaption();

    QPainter painter(this);

    // Source mode clears instead of blending: the store keeps what was drawn last
    // time, and blending the new line and the new caption over the old ones would
    // leave both of each. Only what the repaint asked for is cleared, which
    // setContent() sized to the ground of the old drawing and of the new one
    // together; clearing the whole store every frame would cost more than the
    // drawing does, a store being as large as the whole window while a thumbnail
    // travels home. A store that has just been resized is the one exception,
    // being a new buffer: what is in a part of one that nothing has painted is
    // whatever was in that memory.
    const QRect whole(QPoint(0, 0), size());
    const QRegion stale = m_paintedSize == size() ? event->region() : QRegion(whole);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect &rect : stale) {
        painter.fillRect(rect, Qt::transparent);
    }

    m_painted = drawnBounds() & whole;
    m_paintedSize = size();
    // What the buffer holds from here on, which is what the effect measures its
    // draw against.
    m_shown = m_content;

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setClipRegion(stale);

    // The frame first and the caption over it, the icon and the title being what
    // a corner of the frame gives way to.
    if (m_width > 0.0 && m_strength > 0.0 && m_color.isValid()) {
        QColor color = m_color;
        color.setAlphaF(std::clamp<qreal>(color.alphaF() * m_strength, 0.0, 1.0));

        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(outlinePath(m_corners, m_width), color);
    }

    const QRectF caption = captionRect();
    if (!caption.isEmpty()) {
        // Kept inside the picture it belongs to. The caption is laid out for the
        // size the thumbnail rests at, so a thumbnail passing through a smaller
        // size on its way up to that one is narrower than its own caption, and
        // the part that does not fit would be drawn beside the picture, out of
        // the ground the effect repaints for it. Clipped instead, it is hidden
        // for the moment the trip lasts and whole again the moment the thumbnail
        // arrives, which is where it has to be right.
        painter.setClipRect(m_content, Qt::IntersectClip);

        // All that is left of the caption drawing: it was rendered once, shadows
        // and all, and the fade is the opacity it is stamped at.
        painter.setOpacity(std::min(m_captionOpacity, 1.0));
        painter.drawImage(caption.topLeft(), m_captionImage);
    }
}

// ---------------------------------------------------------------------------
// Click target
// ---------------------------------------------------------------------------

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
            if (m_target) {
                Q_EMIT menuRequested(m_target, m_touchOrigin);
            }
        }
    });
}

ThumbnailOverlay::~ThumbnailOverlay() = default;

void ThumbnailOverlay::setResolver(Resolver resolver) { m_resolver = std::move(resolver); }

void ThumbnailOverlay::cancelTouch() { resetTouch(); }

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
        if (!m_touchArmed && !points.isEmpty()) {
            m_touchId = points.first().id();
            m_touchOrigin = points.first().globalPosition();
            // Settled once, where the finger landed: the answer rides the
            // sequence, so a relayout under it changes nothing.
            m_target = m_resolver ? m_resolver(m_touchOrigin) : nullptr;
            m_touchArmed = m_target != nullptr;
            if (m_touchArmed) {
                m_longPressTimer.start();
            }
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
            if (m_target) {
                Q_EMIT dragStarted(m_target, point.globalPosition(), m_touchId);
            }
            break;
        }
        break;
    case QEvent::TouchEnd:
        if (m_touchArmed) {
            resetTouch();
            if (m_target) {
                Q_EMIT activated(m_target);
            }
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

    // Which thumbnail is asked once, at the press: the window keeps the pointer
    // focus for as long as the button is down, so the rest of the gesture lands
    // here whatever the relayouts in between do to the mask.
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) {
        return;
    }
    m_target = m_resolver ? m_resolver(event->globalPosition()) : nullptr;
    if (!m_target) {
        m_pressed = false;
        return;
    }

    // The left button decides nothing yet: what happens next (a release or a
    // move) is what tells an activation from a drag apart.
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        m_pressOrigin = event->globalPosition();
    } else {
        Q_EMIT menuRequested(m_target, event->globalPosition());
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
    if (m_target) {
        Q_EMIT dragStarted(m_target, event->globalPosition(), -1);
    }
}

void ThumbnailOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
    if (event->button() == Qt::LeftButton && m_pressed) {
        m_pressed = false;
        if (m_target) {
            Q_EMIT activated(m_target);
        }
    }
}

} // namespace ThumbnailBloom
