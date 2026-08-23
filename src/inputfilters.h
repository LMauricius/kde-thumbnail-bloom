/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <input.h>

#include <QList>
#include <QPointer>
#include <QRegion>
#include <QSet>

namespace ThumbnailBloom {

/*!
 * Hands the input a shield captured over to the window really below it.
 *
 * A shield only has to keep its bloomed window from being hovered and clicked;
 * everything else in that area (the windows that became visible under it, the
 * resize borders reaching over it) must keep working. Since the shield wins the
 * hit test, KWin makes it the pointer focus and stops there, so this filter
 * repeats KWin's own focus lookup on every event, skipping the shields and the
 * bloomed windows, and points the focus at what it finds. From there on the
 * event takes its ordinary path: the window below gets the hover, the click
 * raises it, and its decoration gets the resize border.
 *
 * It is installed ahead of the decoration and window action filters, which is
 * where the focus is consumed, but behind the popup and move/resize ones: a
 * click on a shield must still close an open menu and finish a running move.
 */
class ShieldFilter : public KWin::InputEventFilter
{
public:
    ShieldFilter();

    /*! Where one click target is, and whose window it shows. */
    struct Thumbnail
    {
        KWin::Window *window;
        QRegion region;
    };

    /*!
     * Sets what the filter works on, all in logical screen coordinates.
     *
     * \a shields is where the shields are, \a bloomed the windows they hide
     * from the input, and \a thumbnails the click targets: a thumbnail belongs
     * to its own window, so every button that means nothing on a thumbnail is
     * dropped there rather than handed to whatever it is painted over.
     */
    void setState(const QRegion &shields, const QSet<KWin::Window *> &bloomed,
        const QList<Thumbnail> &thumbnails);

    /*!
     * Whether \a window is hidden at \a pos by a window painted over it.
     *
     * A thumbnail is painted at the stacking position of the window it belongs
     * to, so anything above that window covers it. Which window is at \a pos is
     * left to KWin's own hit test, so input shapes and decorations are honoured
     * exactly; only the walk stops early, at the bloomed window itself. Used to
     * keep a thumbnail from acting where it cannot be seen: the input goes to
     * the window covering it, as it does over a shield.
     */
    bool isCovered(KWin::Window *window, const QPointF &pos) const;

    bool pointerMotion(KWin::PointerMotionEvent *event) override;
    bool pointerButton(KWin::PointerButtonEvent *event) override;
    bool pointerAxis(KWin::PointerAxisEvent *event) override;
    bool touchDown(KWin::TouchDownEvent *event) override;

private:
    /*!
     * Moves the focus of \a device off the shield under \a pos, if that is
     * where it is.
     *
     * The pointer and the touch screen keep a focus of their own, but both are
     * InputDeviceHandlers and both are pointed at a shield the same way.
     */
    void redirect(KWin::InputDeviceHandler *device, const QPointF &pos);

    /*! What one walk of the stacking order finds at a point. */
    struct Hit
    {
        KWin::Window *window = nullptr; //!< the window the input belongs to, if any
        bool aboveStop = false; //!< whether it was found before the walk reached the stop window
    };

    /*!
     * Returns the topmost window at \a pos that is neither bloomed nor one of
     * ours, walking the stacking order with KWin's own hit test, and whether it
     * is stacked above \a stopAt (never, when that is null).
     */
    Hit hitAt(const QPointF &pos, const KWin::Window *stopAt) const;
    /*! Returns the thumbnail whose click target holds \a pos, if there is one. */
    const Thumbnail *thumbnailAt(const QPointF &pos) const;
    /*! Whether a thumbnail holds \a pos and is visible there, so that it can act. */
    bool isThumbnailUsable(const QPointF &pos) const;

    QRegion m_shields;
    QList<Thumbnail> m_thumbnails;
    QSet<KWin::Window *> m_bloomed;
};

/*!
 * Keeps a move started on a thumbnail following the finger that started it.
 *
 * KWin's own move filter latches the touch point at the moment it goes down, so
 * a move begun in the middle of a sequence (which is what dragging a thumbnail
 * is: the finger has to travel before the drag is one) is never fed anything.
 * This filter takes that sequence over instead, and it sits ahead of the move
 * filter so the events reach it whatever that one decides to do with them.
 */
class TouchDragFilter : public KWin::InputEventFilter
{
public:
    TouchDragFilter();

    /*! Starts following the point \a id, moving \a window with it. */
    void arm(KWin::Window *window, qint32 id);

    bool touchMotion(KWin::TouchMotionEvent *event) override;
    bool touchUp(KWin::TouchUpEvent *event) override;
    bool touchCancel() override;

private:
    /*! Stops following the sequence, ending the move as \a cancel asks. */
    void disarm(bool cancel);

    QPointer<KWin::Window> m_window;
    qint32 m_id = -1;
};

} // namespace ThumbnailBloom
