/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <core/inputdevice.h>
#include <input.h>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QRectF>
#include <QRegion>
#include <QSet>
#include <QTimer>

#include <functional>

namespace KWin {
class SurfaceInterface;
}

namespace ThumbnailBloom {

/*!
 * Puts pointer and touch input into a window that does not have the input focus.
 *
 * A bloomed window never has it: the click target over its thumbnail is an
 * internal window and wins every hit test, and where the window really is it is
 * covered by whatever made it bloom. Everything a thumbnail passes on (a scroll,
 * a middle click, a touchpad gesture, a finger) therefore has to be handed to
 * the client by hand, which is what this does: it drives KWin's SeatInterface,
 * the compositor's own client-facing input state, entering the target surface,
 * sending the event and leaving again.
 *
 * Two rules keep that from tearing the seat apart. It only ever enters a surface
 * the compositor has already left, which is exactly what happens the moment the
 * pointer lands on a click target and the focus becomes an internal window; and
 * it only ever leaves a focus that is still the one it took, so a focus KWin has
 * set for itself in the meantime is left alone.
 *
 * The position is the caller's to choose, and it is never the pointer's own: the
 * thumbnail is a picture of the window somewhere else entirely, so an event on
 * it means something at the matching spot of the real window, or (for anything
 * that acts on the view as a whole) at its centre.
 *
 * An X11-only session has no seat at all, and there every call does nothing.
 */
class InputForwarder
{
public:
    ~InputForwarder();

    /*! Whether there is a seat to forward to at all. */
    static bool available();

    /*! Returns the centre of the client area of \a window, in global coordinates. */
    static QPointF windowCentre(const KWin::Window *window);

    // -- pointer ----------------------------------------------------------

    /*! The window the forwarded pointer is on, or null. */
    KWin::Window *pointerWindow() const;
    /*! Whether a button sent from here is still down. */
    bool hasButtons() const;
    /*! Whether a touchpad gesture sent from here is still running. */
    bool hasGesture() const;
    /*!
     * Whether the pointer is following the cursor after a click.
     *
     * A button can leave the window in a mode the pointer drives rather than
     * doing something and being over with, so from a click until the pointer is
     * taken off the window again it keeps being told where the pointer goes.
     */
    bool isFollowing() const;

    /*!
     * Puts the forwarded pointer on \a window at \a globalPos.
     *
     * Entering is skipped when the pointer is already there, so this can be
     * called on every event; \a globalPos is then only a move, and not even that
     * while a button or a click of its own is holding the pointer somewhere.
     * Nothing happens at all while the compositor has a focus of its own on some
     * other surface.
     */
    void enter(KWin::Window *window, const QPointF &globalPos);
    /*! Moves the forwarded pointer to \a globalPos, if it is on a window. */
    void motion(const QPointF &globalPos);
    /*! Takes the forwarded pointer off the window it is on, if the seat still has it there. */
    void leave();

    /*! Sends \a button in \a state to the window the pointer is on. */
    void button(Qt::MouseButton button, KWin::PointerButtonState state);
    /*! Sends the scroll \a event to the window the pointer is on. */
    void axis(const KWin::PointerAxisEvent *event);

    void swipeBegin(int fingerCount);
    void swipeUpdate(const QPointF &delta);
    void swipeEnd(bool cancelled);
    void pinchBegin(int fingerCount);
    void pinchUpdate(const QPointF &delta, qreal scale, qreal rotation);
    void pinchEnd(bool cancelled);
    void holdBegin(int fingerCount);
    void holdEnd(bool cancelled);

    // -- touch ------------------------------------------------------------

    /*! The window the forwarded touch points are on, or null. */
    KWin::Window *touchWindow() const;
    /*! Whether the point \a id is one of the forwarded ones. */
    bool isTouchForwarded(qint32 id) const;

    /*! Puts point \a id down on \a window at \a globalPos. */
    void touchDown(KWin::Window *window, qint32 id, const QPointF &globalPos);
    /*! Moves the forwarded point \a id to \a globalPos. */
    void touchMotion(qint32 id, const QPointF &globalPos);
    /*! Lifts the forwarded point \a id. */
    void touchUp(qint32 id);
    /*! Drops every forwarded point without a finish. */
    void touchCancel();

private:
    /*! Whether the seat focus is still the one this put there. */
    bool holdsPointer() const;
    /*! Stamps the seat with the current time, which is what every notify call needs. */
    static void stampTime();

    QPointer<KWin::Window> m_pointerWindow;
    QPointer<KWin::SurfaceInterface> m_pointerSurface;
    QPointF m_pointerPos;
    Qt::MouseButtons m_buttons;
    bool m_gesture = false; //!< whether a touchpad gesture is being forwarded
    bool m_follow = false; //!< whether a click has the pointer following the cursor

    QPointer<KWin::Window> m_touchWindow;
    QSet<qint32> m_touchIds;
};

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
 *
 * The same place in the chain is where a thumbnail's own input is decided, since
 * everything reaching a click target passes through here first. The left and the
 * right button are the two the thumbnail keeps for itself and are handed on to
 * it; everything else is put into the window the thumbnail shows, through an
 * InputForwarder, at the spot of that window the event landed on.
 */
class ShieldFilter : public KWin::InputEventFilter
{
public:
    ShieldFilter();

    /*! Where one click target is, and whose window it shows. */
    struct Thumbnail
    {
        KWin::Window *window;
        //! What the thumbnail answers for: its resting rectangle, or the whole enlarged one
        //! once a click has landed on it
        QRegion region;
        QRectF rect; //!< the whole thumbnail as it is drawn, which is what maps onto the window
    };

    /*!
     * Sets what the filter works on, all in logical screen coordinates.
     *
     * \a shields is where the shields are, \a bloomed the windows they hide
     * from the input, and \a thumbnails the click targets: a thumbnail belongs
     * to its own window, so every event that means nothing on a thumbnail goes
     * to that window rather than to whatever it is painted over.
     *
     * \a backdrops are the maximized windows the thumbnails are painted over
     * instead of around. They are ordinary windows for their own input, but they
     * hide no thumbnail of a window below them, since the paint pass draws those
     * on top of them.
     */
    void setState(const QRegion &shields, const QSet<KWin::Window *> &bloomed,
        const QList<Thumbnail> &thumbnails, const QSet<KWin::Window *> &backdrops);

    /*!
     * Whether \a window is hidden at \a pos by a window painted over it.
     *
     * A thumbnail is painted at the stacking position of the window it belongs
     * to, so anything above that window covers it, the backdrops excepted: those
     * are the one thing the thumbnails are drawn over. Which window is at \a pos
     * is left to KWin's own hit test, so input shapes and decorations are
     * honoured exactly; only the walk stops early, at the bloomed window itself.
     * Used to keep a thumbnail from acting where it cannot be seen: the input
     * goes to the window covering it, as it does over a shield.
     */
    bool isCovered(KWin::Window *window, const QPointF &pos) const;

    /*!
     * Sets what to call when a thumbnail's touch sequence is taken away from its
     * click target, so that the click target can forget the point it was
     * following and make neither a tap nor a hold nor a drag out of it.
     */
    void setTouchTakenOverHandler(std::function<void(KWin::Window *)> handler);

    /*!
     * Sets what to call when a click has been put into the window a thumbnail
     * shows, so that the thumbnail can widen the pointer's hold on it to the
     * enlarged rectangle it is drawn at.
     */
    void setClickHandler(std::function<void(KWin::Window *)> handler);

    /*! Returns the thumbnail that can act at \a pos, if there is one. */
    const Thumbnail *usableThumbnailAt(const QPointF &pos) const;

    bool pointerMotion(KWin::PointerMotionEvent *event) override;
    bool pointerButton(KWin::PointerButtonEvent *event) override;
    bool pointerAxis(KWin::PointerAxisEvent *event) override;

    bool swipeGestureBegin(KWin::PointerSwipeGestureBeginEvent *event) override;
    bool swipeGestureUpdate(KWin::PointerSwipeGestureUpdateEvent *event) override;
    bool swipeGestureEnd(KWin::PointerSwipeGestureEndEvent *event) override;
    bool swipeGestureCancelled(KWin::PointerSwipeGestureCancelEvent *event) override;
    bool pinchGestureBegin(KWin::PointerPinchGestureBeginEvent *event) override;
    bool pinchGestureUpdate(KWin::PointerPinchGestureUpdateEvent *event) override;
    bool pinchGestureEnd(KWin::PointerPinchGestureEndEvent *event) override;
    bool pinchGestureCancelled(KWin::PointerPinchGestureCancelEvent *event) override;
    bool holdGestureBegin(KWin::PointerHoldGestureBeginEvent *event) override;
    bool holdGestureEnd(KWin::PointerHoldGestureEndEvent *event) override;
    bool holdGestureCancelled(KWin::PointerHoldGestureCancelEvent *event) override;

    bool touchDown(KWin::TouchDownEvent *event) override;
    bool touchMotion(KWin::TouchMotionEvent *event) override;
    bool touchUp(KWin::TouchUpEvent *event) override;
    bool touchCancel() override;

private:
    /*! What one walk of the stacking order finds at a point. */
    struct Hit
    {
        KWin::Window *window = nullptr; //!< the window the input belongs to, if any
        bool aboveStop = false; //!< whether it was found before the walk reached the stop window
    };

    /*!
     * Moves the focus of \a device off the shield under \a pos, if that is
     * where it is.
     *
     * The pointer and the touch screen keep a focus of their own, but both are
     * InputDeviceHandlers and both are pointed at a shield the same way.
     */
    void redirect(KWin::InputDeviceHandler *device, const QPointF &pos);
    /*!
     * The same, for a caller that has already walked the stack: \a thumbnail is
     * the one whose click target holds \a pos and \a hit what that walk found.
     */
    void redirect(KWin::InputDeviceHandler *device, const QPointF &pos,
        const Thumbnail *thumbnail, const Hit &hit);

    /*!
     * Returns the topmost window at \a pos that is neither bloomed nor one of
     * ours, walking the stacking order with KWin's own hit test, and whether it
     * is stacked above \a stopAt (never, when that is null).
     */
    Hit hitAt(const QPointF &pos, const KWin::Window *stopAt) const;
    /*! Returns the thumbnail whose click target holds \a pos, if there is one. */
    const Thumbnail *thumbnailAt(const QPointF &pos) const;
    /*! Returns the thumbnail of \a window, if it still has one. */
    const Thumbnail *thumbnailOf(const KWin::Window *window) const;

    /*!
     * Returns where on the real window \a pos of \a thumbnail is, in global
     * coordinates.
     *
     * A thumbnail is the window drawn smaller, so a point on it stands for the
     * point of the window under it. The result is not held inside the window:
     * the thumbnail is clipped by whatever covers it while the window keeps its
     * whole size, so a point near the edge of what is left of a thumbnail can
     * still mean a point past the edge of the window, and the client is told so.
     */
    static QPointF mapToWindow(const Thumbnail &thumbnail, const QPointF &pos);

    /*!
     * Puts the forwarded pointer where \a pos says it belongs, or takes it away.
     *
     * The pointer rests at the centre of the window for as long as its thumbnail
     * is hovered: the window is not really under the pointer, so a position that
     * moved with it would tell the client about a hover it is not having, while
     * a pointer that is simply there is what makes a scroll land. A button
     * changes that, since a press is at a place: from the press until the last
     * button comes up the pointer follows along, at the matching spot of the
     * window.
     */
    void updateForwardedPointer(const QPointF &pos, const Thumbnail *thumbnail);

    /*! Starts a touchpad gesture on the thumbnail under the pointer, if there is one. */
    bool beginGesture();

    /*! One touch point that went down on a thumbnail and was left to its click target. */
    struct PendingTouch
    {
        qint32 id;
        QPointF pos;
        KWin::Window *window;
    };

    QRegion m_shields;
    QList<Thumbnail> m_thumbnails;
    QSet<KWin::Window *> m_bloomed;
    QSet<KWin::Window *> m_backdrops;

    InputForwarder m_forwarder;
    QList<PendingTouch> m_pendingTouches;
    std::function<void(KWin::Window *)> m_touchTakenOver;
    std::function<void(KWin::Window *)> m_clicked;
};

/*!
 * Lets a drag and drop reach the window a thumbnail stands for.
 *
 * KWin routes a drag itself, from a filter that runs before the effects and
 * swallows every event of it, so the thumbnails are invisible to it: the drop
 * would land on the click target, an internal window that wants none. This one
 * is installed ahead of that filter and answers for the thumbnails alone,
 * pointing the drag at the window whose thumbnail the pointer (or the finger) is
 * over. The drop goes to the centre of that window, like a scroll: the window is
 * elsewhere and nothing about the thumbnail says where inside it the drop
 * belongs.
 *
 * Resting on a thumbnail is also how the window itself is asked for. After a
 * while of the drag not going anywhere the window is activated, which brings it
 * out from under whatever hid it and ends its bloom; the drag then carries on
 * over the real window, in KWin's own hands again.
 */
class DragDropFilter : public KWin::InputEventFilter
{
public:
    explicit DragDropFilter(const ShieldFilter &shields);

    /*!
     * Sets how long a drag has to rest on a thumbnail before its window is
     * activated, in milliseconds; zero never activates one.
     */
    void setActivationDelay(int msec);
    /*! Sets what to call when a drag has rested that long on a window. */
    void setActivationHandler(std::function<void(KWin::Window *)> handler);

    bool pointerMotion(KWin::PointerMotionEvent *event) override;
    bool touchMotion(KWin::TouchMotionEvent *event) override;

private:
    /*!
     * Points a running drag at the thumbnail under \a pos, and says whether one
     * took it. \a movePointer tells whether the pointer itself has to be moved
     * along, which is what keeps the drag icon under it.
     */
    bool dragOnto(const QPointF &pos, bool movePointer);
    /*! Starts, keeps or drops the wait that activates \a window. */
    void dwellOn(KWin::Window *window);

    const ShieldFilter &m_shields;
    QTimer m_dwellTimer;
    QPointer<KWin::Window> m_dwellWindow;
    int m_delay = 0;
    std::function<void(KWin::Window *)> m_activate;
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
