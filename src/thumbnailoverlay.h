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
#include <QSizeF>
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
 *
 * Neither of those two is ever drawn. The effect leaves every surface of its own
 * out of the pass (ThumbnailBloomEffect::paintWindow()) and draws back only the
 * one that has something on it, which is ThumbnailCanvas; a window that paints
 * nothing but a transparent buffer would otherwise still be blended over the
 * damage on every frame. The buffer behind one of these is therefore allocated
 * once, cleared once, and never looked at again.
 */
class OverlayWindow : public QRasterWindow
{
    Q_OBJECT

public:
    OverlayWindow();
    ~OverlayWindow() override;

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
};

/*!
 * Everything a thumbnail has drawn on it: the frame around the picture and the
 * caption (the icon and the title) inside it.
 *
 * A window rather than something the effect stamps on the screen, because a
 * thumbnail can be painted twice in a pass (the lift) and can be missed by a
 * partial repaint, both of which show on anything drawn over it by hand. The
 * compositor draws this one once per frame like any other surface, and the
 * antialiasing of the frame is QPainter's. It is the only surface of the effect
 * that is drawn at all: the click target and the shield are input and nothing
 * else, so putting the caption here is what keeps a thumbnail down to a single
 * blended quad.
 *
 * The window is a store to paint into rather than the drawing itself. The effect
 * keeps it at the largest rectangle the running animation will draw plus the
 * margin the line needs, moves it onto the thumbnail every frame and draws it
 * untransformed, and the line is painted around the part of it the thumbnail
 * currently covers. Nothing is ever scaled that way, so the frame around a
 * thumbnail grown under the pointer is as sharp as the one around a thumbnail at
 * rest. The bend is painted rather than transformed for the same reason: the
 * corners handed to setContent() are already the bent ones, at the size of this
 * very frame.
 *
 * The line lies outside the thumbnail rather than on it, so that it hides
 * nothing of the picture, and it is drawn over the thumbnail all the same: the
 * shadow of a window is painted with it and reaches further out than the frame
 * does. The corners handed to setContent() are on the physical pixel grid and
 * the width is a whole number of pixels, so the line comes out sharp; the
 * thumbnail underneath is bled half a pixel outwards to meet it, the picture
 * being the one of the two that can be stretched unnoticed.
 *
 * Sharp on the inside and rounded on the outside, by the width of the frame
 * itself: the picture keeps its corners, and the frame around them is the same
 * thickness at a corner as it is along an edge.
 *
 * The caption rides the picture: it is laid out for the size the thumbnail rests
 * at and put on the bottom of wherever the thumbnail is drawn at this moment, so
 * a trip carries it along instead of leaving it at the destination. Laying it
 * out against the travelling rectangle would mean drawing it again, shadows and
 * all, on every frame of every trip. At rest the two rectangles are the same one.
 *
 * It takes no input at all, the click target below it answering for the whole
 * thumbnail.
 */
class ThumbnailCanvas : public OverlayWindow
{
    Q_OBJECT

public:
    ThumbnailCanvas();

    /*!
     * Sets what the caption shows: \a icon beside \a title, either of which may
     * be empty to leave that part out, laid out for a thumbnail of \a restSize.
     *
     * The size is the one the thumbnail comes to rest at rather than the one it
     * is drawn at now, since the caption is laid out once and carried along a
     * trip rather than laid out again for every size the picture passes through.
     */
    void setCaption(const QIcon &icon, const QString &title, const QSizeF &restSize);

    /*!
     * Sets everything this frame of the animation draws.
     *
     * \a content is the part of the store the thumbnail covers. The frame is a
     * band \a width logical pixels wide lying outside the four \a corners, which
     * are its inside in window coordinates, clockwise from the top left, drawn
     * in \a color at \a strength of its full opacity; the caption is stamped at
     * \a captionOpacity.
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
    void setContent(const QRectF &content, const std::array<QPointF, 4> &corners, qreal width,
        const QColor &color, qreal strength, qreal captionOpacity);

    /*!
     * Returns the rectangle the drawing now in the store was painted around, in
     * window coordinates, which is empty until something has been.
     *
     * It is what the buffer actually holds rather than what was last asked for,
     * so the effect can tell whether the two have come apart and put the draw
     * right if they have.
     */
    QRectF shownRect() const;

    /*!
     * Returns the part of the store the drawing now in it occupies, in window
     * coordinates: the band the frame covers and the strip the caption sits on,
     * never the picture between them, which the store never touches.
     *
     * It is what the effect clips the draw of the store to. A store is as large
     * as the thumbnail it belongs to and holds ink around the edge of it alone,
     * so blending the whole of it would cost the screen over again for every
     * thumbnail on it.
     */
    QRegion paintedRegion() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /*!
     * Where in the store the rendered caption goes: hung from the bottom centre
     * of the picture, snapped to the physical pixel grid so that stamping it
     * resamples nothing. Empty when there is nothing to stamp.
     */
    QRectF captionRect() const;
    /*!
     * Draws the caption into m_captionImage, unless that one is still good.
     *
     * The two shadows are blurred on the processor, which is far too much work
     * to repeat on every frame of a fade, let alone of a trip, so the result is
     * kept and only stamped from then on.
     */
    void renderCaption();
    /*! Drops the rendered caption, so that the next paint makes it again. */
    void invalidateCaption();
    /*!
     * Everything a paint would put on the store as it stands, in window
     * coordinates: the band of the frame and the strip of the caption.
     *
     * A region rather than the rectangle around the two, because the rectangle
     * around them is the whole thumbnail and the ink is a line along the edge of
     * it. Everything the store costs per frame is measured from this: the pixels
     * the paint clears, the pixels Qt uploads afterwards, and the pixels the
     * scene blends when the effect draws the result.
     */
    QRegion drawnBounds() const;

    QRectF m_content; //!< part of the store the thumbnail covers, as last asked for
    QRectF m_shown; //!< the same, as the buffer now holds it
    QSizeF m_restSize; //!< size the thumbnail comes to rest at, which the caption is laid out for
    std::array<QPointF, 4> m_corners {};
    qreal m_width = 0.0;
    QColor m_color;
    qreal m_strength = 0.0;

    QIcon m_icon;
    QString m_title;
    qreal m_captionOpacity = 0.0;
    QImage m_captionImage; //!< the caption as drawn, in device pixels, or null for none
    //! Where the image hangs from the bottom centre of the picture, the shadows included.
    QRectF m_captionBand;
    bool m_captionDirty = true; //!< whether the image still matches the caption and the rest size

    QRegion m_painted; //!< what the last paint drew into, in window coordinates
    QSize m_paintedSize; //!< size of the store it drew into, a new one being a new buffer
};

/*!
 * The click target placed exactly on top of a thumbnail.
 *
 * It covers the thumbnail's rectangle and turns the gestures it swallows into
 * the three things a thumbnail can do: activate its window, drag it out of the
 * thumbnail, or open its window menu. It draws nothing whatsoever: the picture
 * is the effect's, and the frame and the caption over it belong to
 * ThumbnailCanvas.
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
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    /*! Whether \a pos is far enough from \a origin to count as a drag. */
    static bool isDrag(const QPointF &origin, const QPointF &pos);
    /*! Ends the tracked touch sequence: no tap or long press can come of it any more. */
    void resetTouch();

    QPointF m_pressOrigin; //!< where the left button went down
    bool m_pressed = false; //!< whether a left press is still undecided

    QTimer m_longPressTimer;
    QPointF m_touchOrigin; //!< where the tracked touch point went down
    qint32 m_touchId = -1; //!< point id of the sequence being followed
    bool m_touchArmed = false; //!< whether the sequence can still tap or long press
};

} // namespace ThumbnailBloom
