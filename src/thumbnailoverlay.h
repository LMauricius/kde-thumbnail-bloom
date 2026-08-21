/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QIcon>
#include <QPointF>
#include <QRasterWindow>
#include <QString>
#include <QTimer>

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
 * The click target placed exactly on top of a thumbnail.
 *
 * It covers the thumbnail's rectangle and turns the gestures it swallows into
 * the three things a thumbnail can do: activate its window, drag it out of the
 * thumbnail, or open its window menu. The hover outline is drawn by the effect
 * instead, so that it stays in step with the animation.
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
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    /*! Whether \a pos is far enough from \a origin to count as a drag. */
    static bool isDrag(const QPointF &origin, const QPointF &pos);

    QIcon m_icon;
    QString m_title;
    qreal m_captionOpacity = 0.0;

    QPointF m_pressOrigin; //!< where the left button went down
    bool m_pressed = false; //!< whether a left press is still undecided

    QTimer m_longPressTimer;
    QPointF m_touchOrigin; //!< where the tracked touch point went down
    qint32 m_touchId = -1; //!< point id of the sequence being followed
    bool m_touchArmed = false; //!< whether the sequence can still tap or long press
};

} // namespace ThumbnailBloom
