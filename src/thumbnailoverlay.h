/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPointF>
#include <QRasterWindow>
#include <QRectF>
#include <QRegion>
#include <QString>
#include <QTimer>

#include <array>

namespace ThumbnailBloom {

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

    /*!
     * Makes the window paint without taking any input, or stop doing so.
     *
     * KWin reads the "outputOnly" property of the QWindow in
     * InternalWindow::hitTest(), which is the only way an internal window can
     * stay on screen and yet be missed by the hit test; hiding it would take
     * the painting with it. Subclasses also stop reporting gestures, so an
     * event that reaches one anyway still means nothing.
     */
    void setOutputOnly(bool outputOnly);

protected:
    /*! Whether the window is painting only and must report no gesture. */
    bool isOutputOnly() const;

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
};

/*!
 * The frame drawn around a thumbnail.
 *
 * A window that paints rather than something the effect stamps on the screen,
 * for the same reason the caption is one: the compositor draws it once per
 * frame like any other surface, so it can neither be missed by a partial
 * repaint nor drawn where the thumbnail is not, and it needs no shader of its
 * own to be antialiased.
 *
 * The window is a store to paint frames into rather than the frame itself. The
 * effect keeps it at the largest rectangle the running animation will draw plus
 * the margin the line needs, moves it onto the thumbnail every frame and draws
 * it untransformed, and the line is painted around the part of it the thumbnail
 * currently covers. Nothing is ever scaled that way, so the frame around a
 * thumbnail grown under the pointer is as sharp as the one around a thumbnail
 * at rest. The bend is painted rather than transformed for the same reason: the
 * corners handed to setOutline() are already the bent ones, at the size of this
 * very frame.
 *
 * The line lies outside the thumbnail rather than on it, so that it hides
 * nothing of the picture, and it is drawn over the thumbnail all the same: the
 * shadow of a window is painted with it and reaches further out than the frame
 * does. The corners handed to setOutline() are on the physical pixel grid and
 * the width is a whole number of pixels, so the line comes out sharp; the
 * thumbnail underneath is bled half a pixel outwards to meet it, the picture
 * being the one of the two that can be stretched unnoticed.
 *
 * Sharp on the inside and rounded on the outside, by the width of the frame
 * itself: the picture keeps its corners, and the frame around them is the same
 * thickness at a corner as it is along an edge.
 *
 * It takes no input at all, the click target below it answering for the whole
 * thumbnail.
 */
class OutlineOverlay : public OverlayWindow
{
    Q_OBJECT

public:
    OutlineOverlay();

    /*!
     * Sets the frame to draw around \a content, the part of the store the
     * thumbnail covers: the four \a corners of the inside of the frame in window
     * coordinates, clockwise from the top left, a band \a width logical pixels
     * wide outside them, in \a color, at \a strength of its full opacity.
     *
     * The corners are the inside of the frame and not its middle, so the whole
     * of the band lies outside them. They are the picture's own corners, sharp,
     * while the outside of each one is rounded by the width of the band.
     *
     * A change too small to be seen is dropped rather than repainted, since
     * every frame of a hover offers a slightly different one. Anything else is
     * painted before this returns, not when the event loop next comes round: the
     * compositor is in the middle of drawing the very step of the animation this
     * frame belongs to.
     */
    void setOutline(const QRectF &content, const std::array<QPointF, 4> &corners, qreal width,
        const QColor &color, qreal strength);

    /*!
     * Returns the rectangle the frame now in the store was painted around, in
     * window coordinates, which is empty until one has been.
     *
     * It is what the buffer actually holds rather than what was last asked for,
     * so the effect can tell whether the two have come apart and put the draw
     * right if they have.
     */
    QRectF shownRect() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRectF m_content; //!< part of the store the thumbnail covers, as last asked for
    QRectF m_shown; //!< the same, as the buffer now holds it
    std::array<QPointF, 4> m_corners {};
    qreal m_width = 0.0;
    QColor m_color;
    qreal m_strength = 0.0;
    QRect m_painted; //!< what the last paint drew into, in window coordinates
    QSize m_paintedSize; //!< size of the store it drew into, a new one being a new buffer
};

/*!
 * The click target placed exactly on top of a thumbnail.
 *
 * It covers the thumbnail's rectangle and turns the gestures it swallows into
 * the three things a thumbnail can do: activate its window, drag it out of the
 * thumbnail, or open its window menu. The frame around the thumbnail is a window
 * of its own (OutlineOverlay), since it has to follow the animation.
 *
 * It is also what carries the icon and the title of the window. Painting those
 * here rather than from the effect is what keeps them stable: the compositor
 * draws this window once per frame like any other, so the caption can neither
 * be missed by a partial repaint nor blended over itself when the thumbnail is
 * painted twice (which is what a lifted thumbnail is).
 *
 * A press never decides anything on its own; only what follows it does. The
 * pointer keeps its focus on this window for as long as a button is down
 * (PointerInputRedirection blocks focus updates then), so the whole gesture is
 * seen here, and a touch sequence is followed by its point id.
 */
class ThumbnailOverlay : public OverlayWindow
{
    Q_OBJECT

public:
    ThumbnailOverlay();
    ~ThumbnailOverlay() override;

    /*!
     * Sets what the caption shows: \a icon above \a title, either of which may
     * be empty to leave that part out.
     */
    void setCaption(const QIcon &icon, const QString &title);
    /*! Sets how opaque the caption is painted, from 0 (gone) to 1. */
    void setCaptionOpacity(qreal opacity);

    /*!
     * Gives up the touch sequence being followed, so that nothing comes of it.
     *
     * Called when a second finger lands on the thumbnail and the gesture turns
     * out to be one for the window itself: the point this was following is put
     * into the window along with the new one, and no tap, hold or drag may come
     * of it here any more.
     */
    void cancelTouch();

Q_SIGNALS:
    /*! A click or a tap finished on the thumbnail without turning into a drag. */
    void activated();
    /*!
     * The press on the thumbnail travelled far enough to become a move.
     *
     * \a pos is where the pointer or the finger is now, and \a touchId the id
     * of the touch point driving it, or -1 for the pointer.
     */
    void dragStarted(const QPointF &pos, qint32 touchId);
    /*! The window menu was asked for at \a pos, by a right click or a long touch. */
    void menuRequested(const QPointF &pos);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    /*! Whether \a pos is far enough from \a origin to count as a drag. */
    static bool isDrag(const QPointF &origin, const QPointF &pos);
    /*! Ends the tracked touch sequence: no tap or long press can come of it any more. */
    void resetTouch();
    /*!
     * Draws the caption into m_captionImage, unless that one is still good.
     *
     * The two shadows are blurred on the processor, which is far too much work
     * to repeat on every frame of a fade, so the result is kept and only
     * stamped from then on.
     */
    void renderCaption();
    /*! Drops the rendered caption and repaints, so that the next paint makes it again. */
    void invalidateCaption();

    QIcon m_icon;
    QString m_title;
    qreal m_captionOpacity = 0.0;

    QImage m_captionImage; //!< the caption as drawn, in device pixels, or null for none
    QRectF m_captionBand; //!< where in the window the image goes, the shadows included
    bool m_captionDirty = true; //!< whether the image still matches the caption and the window

    QPointF m_pressOrigin; //!< where the left button went down
    bool m_pressed = false; //!< whether a left press is still undecided

    QTimer m_longPressTimer;
    QPointF m_touchOrigin; //!< where the tracked touch point went down
    qint32 m_touchId = -1; //!< point id of the sequence being followed
    bool m_touchArmed = false; //!< whether the sequence can still tap or long press
};

} // namespace ThumbnailBloom
