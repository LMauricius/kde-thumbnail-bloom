/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inputfilters.h"

#include <input_event.h>
#include <pointer_input.h>
#include <touch_input.h>
#include <wayland/seat.h>
#include <wayland/surface.h>
#include <wayland_server.h>
#include <window.h>
#include <workspace.h>

#include <algorithm>
#include <chrono>

using namespace KWin;

namespace ThumbnailBloom {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/*! Returns the seat every forwarded event goes through, or null on X11. */
static SeatInterface *seat()
{
    // An X11-only session runs no Wayland server at all, and nothing here can be
    // done through the X server instead: the events would have to be replayed
    // into it from the outside.
    WaylandServer *server = waylandServer();
    return server ? server->seat() : nullptr;
}

/*!
 * Stamps the seat with the current time.
 *
 * Every event handed to a client carries one, and clients measure double clicks
 * and drag thresholds by it. The clock is the one libinput timestamps come from,
 * so a forwarded event sits in the same timeline as a real one.
 */
static void stampSeat()
{
    if (SeatInterface *s = seat()) {
        s->setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()));
    }
}

// ---------------------------------------------------------------------------
// InputForwarder
// ---------------------------------------------------------------------------

InputForwarder::~InputForwarder()
{
    leave();
    touchCancel();
}

bool InputForwarder::available() { return seat() != nullptr; }

QPointF InputForwarder::windowCentre(const Window *window)
{
    // The client area rather than the frame: the middle of a window that is
    // mostly decoration would otherwise land on the decoration, which takes its
    // input from KWin and not from the client at all.
    const QRectF client = window->clientGeometry();
    return client.isEmpty() ? QRectF(window->frameGeometry()).center() : client.center();
}

bool InputForwarder::holdsPointer() const
{
    SeatInterface *s = seat();
    if (!s || !m_pointerWindow || !m_pointerSurface
        || s->focusedPointerSurface() != m_pointerSurface) {
        return false;
    }

    // The surface being the one this entered is not enough: the compositor may
    // have taken the very same window for its own focus since (the window of a
    // thumbnail that was clicked gets activated and raised, and the pointer ends
    // up on it for real). The seat focus is KWin's then, not this one's, and
    // leaving it would take away a focus the window is entitled to without ever
    // sending another enter, which leaves that window deaf to everything. While
    // the window is really blooming this can never be true: a bloomed window is
    // skipped by every focus lookup, KWin's own included.
    return input()->pointer()->focus() != m_pointerWindow;
}

Window *InputForwarder::pointerWindow() const
{
    return holdsPointer() ? m_pointerWindow.data() : nullptr;
}

bool InputForwarder::hasButtons() const { return holdsPointer() && m_buttons != Qt::NoButton; }

bool InputForwarder::hasGesture() const { return holdsPointer() && m_gesture; }

bool InputForwarder::isFollowing() const { return holdsPointer() && m_follow; }

void InputForwarder::enter(Window *window, const QPointF &globalPos)
{
    SeatInterface *s = seat();
    if (!s || !window) {
        return;
    }

    // Already on it: only the position is left to say, and a button (or the click
    // that armed the follow) holds that where the caller put it.
    if (holdsPointer() && m_pointerWindow == window) {
        if (m_buttons == Qt::NoButton && !m_follow) {
            motion(globalPos);
        }
        return;
    }

    // Nothing is ever taken from the compositor: the focus is entered only where
    // KWin has none, which is exactly what a click target winning the hit test
    // leaves behind, since an internal window has no surface to focus. A drag is
    // somebody else's business as well (see DragDropFilter), and the seat is in
    // no state to be entered while one runs.
    SurfaceInterface *surface = window->surface();
    if (!surface || s->isDrag() || s->focusedPointerSurface()) {
        return;
    }

    stampSeat();
    s->notifyPointerEnter(surface, globalPos, window->inputTransformation());
    s->notifyPointerFrame();

    m_pointerWindow = window;
    m_pointerSurface = surface;
    m_pointerPos = globalPos;
    m_buttons = Qt::NoButton;
    m_gesture = false;
    m_follow = false;
}

void InputForwarder::motion(const QPointF &globalPos)
{
    if (!holdsPointer() || globalPos == m_pointerPos) {
        return;
    }

    stampSeat();
    seat()->notifyPointerMotion(globalPos);
    seat()->notifyPointerFrame();
    m_pointerPos = globalPos;
}

void InputForwarder::leave()
{
    // Only a focus that is still the one this put there is given up. Once KWin
    // has entered another surface for itself the seat has already sent the leave
    // (entering a surface leaves the one before it), so there is nothing left to
    // do but forget.
    if (holdsPointer()) {
        stampSeat();
        seat()->notifyPointerLeave();
        seat()->notifyPointerFrame();
    }

    m_pointerWindow.clear();
    m_pointerSurface.clear();
    m_buttons = Qt::NoButton;
    m_gesture = false;
    m_follow = false;
}

void InputForwarder::button(Qt::MouseButton button, PointerButtonState state)
{
    if (!holdsPointer()) {
        return;
    }

    stampSeat();
    seat()->notifyPointerButton(button, state);
    seat()->notifyPointerFrame();

    if (state == PointerButtonState::Pressed) {
        m_buttons |= button;

        // From here on the pointer follows the cursor, release or no release.
        // A click is not always something the window does and has done with: it
        // can put the window into a mode the pointer drives from then on, which
        // is what Firefox's middle click autoscroll is, and a window left in one
        // of those with nothing moving inside it does nothing at all.
        m_follow = true;
    } else {
        m_buttons &= ~button;
    }
}

void InputForwarder::axis(const PointerAxisEvent *event)
{
    if (!holdsPointer()) {
        return;
    }

    stampSeat();
    seat()->notifyPointerAxis(
        event->orientation, event->delta, event->deltaV120, event->source, event->inverted);
    seat()->notifyPointerFrame();
}

// A gesture that never began must not be ended, so each of these is guarded by
// the flag the begin sets. Losing the focus in the middle drops the flag with
// everything else, and the client gets the cancel that comes with the leave.

void InputForwarder::swipeBegin(int fingerCount)
{
    if (!holdsPointer()) {
        return;
    }
    stampSeat();
    seat()->startPointerSwipeGesture(fingerCount);
    m_gesture = true;
}

void InputForwarder::swipeUpdate(const QPointF &delta)
{
    if (!hasGesture()) {
        return;
    }
    stampSeat();
    seat()->updatePointerSwipeGesture(delta);
}

void InputForwarder::swipeEnd(bool cancelled)
{
    if (!hasGesture()) {
        return;
    }
    stampSeat();
    if (cancelled) {
        seat()->cancelPointerSwipeGesture();
    } else {
        seat()->endPointerSwipeGesture();
    }
    m_gesture = false;
}

void InputForwarder::pinchBegin(int fingerCount)
{
    if (!holdsPointer()) {
        return;
    }
    stampSeat();
    seat()->startPointerPinchGesture(fingerCount);
    m_gesture = true;
}

void InputForwarder::pinchUpdate(const QPointF &delta, qreal scale, qreal rotation)
{
    if (!hasGesture()) {
        return;
    }
    stampSeat();
    seat()->updatePointerPinchGesture(delta, scale, rotation);
}

void InputForwarder::pinchEnd(bool cancelled)
{
    if (!hasGesture()) {
        return;
    }
    stampSeat();
    if (cancelled) {
        seat()->cancelPointerPinchGesture();
    } else {
        seat()->endPointerPinchGesture();
    }
    m_gesture = false;
}

void InputForwarder::holdBegin(int fingerCount)
{
    if (!holdsPointer()) {
        return;
    }
    stampSeat();
    seat()->startPointerHoldGesture(fingerCount);
    m_gesture = true;
}

void InputForwarder::holdEnd(bool cancelled)
{
    if (!hasGesture()) {
        return;
    }
    stampSeat();
    if (cancelled) {
        seat()->cancelPointerHoldGesture();
    } else {
        seat()->endPointerHoldGesture();
    }
    m_gesture = false;
}

Window *InputForwarder::touchWindow() const { return m_touchWindow.data(); }

bool InputForwarder::isTouchForwarded(qint32 id) const { return m_touchIds.contains(id); }

void InputForwarder::touchDown(Window *window, qint32 id, const QPointF &globalPos)
{
    SeatInterface *s = seat();
    if (!s || !window) {
        return;
    }

    // One window holds the whole sequence: the points of a gesture belong
    // together, and half of a pinch delivered somewhere else is worse than none.
    SurfaceInterface *surface = window->surface();
    if (!surface || (m_touchWindow && m_touchWindow != window)) {
        return;
    }

    stampSeat();
    // The surface position is what the point is measured from, so the client
    // reads the same local coordinates it would for a touch where the window
    // really is.
    s->notifyTouchDown(surface, QRectF(window->bufferGeometry()).topLeft(), id, globalPos);
    s->notifyTouchFrame();

    m_touchWindow = window;
    m_touchIds.insert(id);
}

void InputForwarder::touchMotion(qint32 id, const QPointF &globalPos)
{
    SeatInterface *s = seat();
    if (!s || !m_touchIds.contains(id)) {
        return;
    }

    stampSeat();
    s->notifyTouchMotion(id, globalPos);
    s->notifyTouchFrame();
}

void InputForwarder::touchUp(qint32 id)
{
    SeatInterface *s = seat();
    if (!s || !m_touchIds.remove(id)) {
        return;
    }

    stampSeat();
    s->notifyTouchUp(id);
    s->notifyTouchFrame();

    if (m_touchIds.isEmpty()) {
        m_touchWindow.clear();
    }
}

void InputForwarder::touchCancel()
{
    if (m_touchIds.isEmpty()) {
        return;
    }

    if (SeatInterface *s = seat()) {
        stampSeat();
        s->notifyTouchCancel();
    }

    m_touchIds.clear();
    m_touchWindow.clear();
}

ShieldFilter::ShieldFilter()
    : InputEventFilter(InputFilterOrder::Popup)
{ }

void ShieldFilter::setState(const QRegion &shields, const QSet<Window *> &bloomed,
    const QList<Thumbnail> &thumbnails, const QSet<Window *> &backdrops)
{
    m_shields = shields;
    m_bloomed = bloomed;
    m_thumbnails = thumbnails;
    m_backdrops = backdrops;

    // A thumbnail can go away under a pointer that is not moving (the window
    // comes home, or something opens over it), and then nothing would come along
    // to take the forwarded pointer off the client. A button still down keeps
    // it: that gesture is the client's until it is finished.
    if (!m_forwarder.hasButtons() && !thumbnailOf(m_forwarder.pointerWindow())) {
        m_forwarder.leave();
    }
}

ShieldFilter::Hit ShieldFilter::hitAt(const QPointF &pos, const Window *stopAt) const
{
    // The same walk InputRedirection::findToplevel() does, minus what must not
    // answer here: the bloomed windows, which are not painted where they are,
    // and the internal windows, which are the effect's own shields and click
    // targets. KWin's other internal surfaces are cut out of the shields long
    // before this, so skipping all of them takes nothing away.
    //
    // Both questions the filter has come out of this one walk: which window the
    // input really belongs to, and whether that window is stacked above \a
    // stopAt. \a stopAt is looked for before the skips, since a minimized or
    // bloomed one still holds its place in the stack and only what is genuinely
    // above it may count as being above it.
    Hit hit;
    bool aboveStop = stopAt != nullptr;

    const QList<Window *> &stacking = workspace()->stackingOrder();
    for (auto it = stacking.crbegin(); it != stacking.crend(); ++it) {
        Window *window = *it;
        if (window == stopAt) {
            aboveStop = false;
        }
        if (window->isDeleted() || window->isMinimized() || window->isHidden()
            || window->isHiddenByShowDesktop()) {
            continue;
        }
        if (!window->isOnCurrentActivity() || !window->isOnCurrentDesktop()
            || !window->readyForPainting()) {
            continue;
        }
        if (window->isInternal() || m_bloomed.contains(window)) {
            continue;
        }
        // A backdrop is the one window a thumbnail is painted over from below,
        // so it hides nothing of the one being asked about here. Only above the
        // stop window, since that is the whole span the question covers: further
        // down it is an ordinary window taking its own input.
        if (aboveStop && m_backdrops.contains(window)) {
            continue;
        }
        if (window->hitTest(pos)) {
            hit.window = window;
            hit.aboveStop = aboveStop;
            return hit;
        }
    }

    return hit;
}

bool ShieldFilter::isCovered(Window *window, const QPointF &pos) const
{
    // Whatever answers the hit test before the bloomed window is reached is
    // stacked above it, and so is painted over its thumbnail.
    return window && hitAt(pos, window).aboveStop;
}

const ShieldFilter::Thumbnail *ShieldFilter::thumbnailAt(const QPointF &pos) const
{
    // The click targets never overlap, so the first hit is the only one.
    const auto it = std::find_if(m_thumbnails.cbegin(), m_thumbnails.cend(),
        [&pos](const Thumbnail &thumbnail) { return thumbnail.region.contains(pos.toPoint()); });
    return it != m_thumbnails.cend() ? &(*it) : nullptr;
}

const ShieldFilter::Thumbnail *ShieldFilter::thumbnailOf(const Window *window) const
{
    const auto it = std::find_if(m_thumbnails.cbegin(), m_thumbnails.cend(),
        [window](const Thumbnail &thumbnail) { return thumbnail.window == window; });
    return it != m_thumbnails.cend() ? &(*it) : nullptr;
}

const ShieldFilter::Thumbnail *ShieldFilter::usableThumbnailAt(const QPointF &pos) const
{
    // The pixels a window is painted over belong to that window, so a thumbnail
    // does nothing there, however much of its click target reaches over it.
    const Thumbnail *thumbnail = thumbnailAt(pos);
    return thumbnail && !isCovered(thumbnail->window, pos) ? thumbnail : nullptr;
}

QPointF ShieldFilter::mapToWindow(const Thumbnail &thumbnail, const QPointF &pos)
{
    const QRectF frame = thumbnail.window->frameGeometry();
    if (thumbnail.rect.isEmpty() || frame.isEmpty()) {
        return InputForwarder::windowCentre(thumbnail.window);
    }

    // The thumbnail is the frame of the window drawn smaller, corner to corner,
    // so one scale each way undoes it. The rectangle is the one it is drawn at,
    // which under the pointer is the grown one: a point is aimed at the picture
    // on screen, and measuring it against the smaller resting rectangle would
    // land it further from the centre of the window the further out it is.
    // Nothing is clamped: what is left of a clipped thumbnail still stands for
    // the whole window, and a client is better told about a point past its edge
    // than about the wrong point.
    const QPointF offset = pos - thumbnail.rect.topLeft();
    return frame.topLeft()
        + QPointF(offset.x() * frame.width() / thumbnail.rect.width(),
            offset.y() * frame.height() / thumbnail.rect.height());
}

void ShieldFilter::setTouchTakenOverHandler(std::function<void(Window *)> handler)
{
    m_touchTakenOver = std::move(handler);
}

void ShieldFilter::setClickHandler(std::function<void(Window *)> handler)
{
    m_clicked = std::move(handler);
}

void ShieldFilter::updateForwardedPointer(const QPointF &pos, const Thumbnail *thumbnail)
{
    Window *held = m_forwarder.pointerWindow();

    // A button that went down on a thumbnail owns the pointer until it comes up,
    // wherever the pointer has got to since: the client is in the middle of a
    // gesture of its own and has to be told where it is going. The thumbnail may
    // well have moved out from under it, and then there is nothing left to map
    // against and the centre stands in.
    if (m_forwarder.hasButtons()) {
        const Thumbnail *pressed = thumbnailOf(held);
        m_forwarder.motion(
            pressed ? mapToWindow(*pressed, pos) : InputForwarder::windowCentre(held));
        return;
    }

    // A click keeps the pointer following even after the button comes up: what it
    // started may be a mode the pointer drives rather than something the window
    // has already done (Firefox's middle click autoscroll is the one everybody
    // has), and there is no asking the client which it was. What ends it is the
    // pointer leaving the thumbnail, and by then the thumbnail is the enlarged
    // one, so that is what has to be left: the hold matches what is drawn under
    // the pointer rather than the smaller rectangle underneath it.
    if (m_forwarder.isFollowing()) {
        const Thumbnail *followed = thumbnailOf(held);
        if (followed && followed->region.contains(pos.toPoint())) {
            m_forwarder.motion(mapToWindow(*followed, pos));
            return;
        }
    }

    if (!thumbnail) {
        m_forwarder.leave();
        return;
    }

    // Otherwise nothing moves the forwarded pointer. The window is not under the
    // pointer at all, so a position travelling with it would tell the client
    // about a hover it is not having; it rests in the middle of the window
    // instead, which is where a scroll or a gesture is aimed anyway. Keeping it
    // there for as long as the thumbnail is hovered, rather than putting it down
    // for each event, is what makes a scroll run smoothly.
    m_forwarder.enter(thumbnail->window, InputForwarder::windowCentre(thumbnail->window));
}

bool ShieldFilter::beginGesture()
{
    const Thumbnail *thumbnail = usableThumbnailAt(input()->globalPointer());
    if (!thumbnail) {
        return false;
    }

    m_forwarder.enter(thumbnail->window, InputForwarder::windowCentre(thumbnail->window));
    return m_forwarder.pointerWindow() != nullptr;
}

void ShieldFilter::redirect(InputDeviceHandler *device, const QPointF &pos)
{
    // One walk of the stack answers both halves: what is really under the
    // pointer, and whether it is stacked above the thumbnail the click target
    // belongs to, which is what makes that thumbnail invisible here.
    const Thumbnail *thumbnail = thumbnailAt(pos);
    redirect(device, pos, thumbnail, hitAt(pos, thumbnail ? thumbnail->window : nullptr));
}

void ShieldFilter::redirect(
    InputDeviceHandler *device, const QPointF &pos, const Thumbnail *thumbnail, const Hit &hit)
{
    // Only what one of ours caught is moved on: a shield, or a click target on a
    // part of its thumbnail that a window is painted over. Anything else is
    // already focused where it belongs.
    Window *focus = device->focus();
    if (!focus || !focus->isInternal()) {
        return;
    }

    const bool covered = thumbnail && hit.aboveStop;
    if (!m_shields.contains(pos.toPoint()) && !covered) {
        return;
    }

    // The split KWin makes in updateDecoration() and updateFocus(): outside the
    // client area the decoration takes the event and the window itself is not
    // focused at all, which is what makes the resize borders work.
    Window *below = hit.window;
    Decoration::DecoratedWindowImpl *decoration = nullptr;
    if (below && below->decoratedWindow() && !below->clientGeometry().contains(pos)) {
        decoration = below->decoratedWindow();
    }

    device->setDecoration(decoration);
    device->setFocus(decoration ? nullptr : below);
}

bool ShieldFilter::pointerMotion(PointerMotionEvent *event)
{
    // Motion arrives for every step the pointer takes, so the stacking order is
    // walked once here and the answer handed to both of the jobs that need it.
    const Thumbnail *thumbnail = thumbnailAt(event->position);
    const Hit hit = hitAt(event->position, thumbnail ? thumbnail->window : nullptr);

    // Forwarding comes before the redirect, so that a pointer leaving a
    // thumbnail for a window below is taken off the client it was on before the
    // next one is entered.
    updateForwardedPointer(event->position, hit.aboveStop ? nullptr : thumbnail);
    redirect(input()->pointer(), event->position, thumbnail, hit);
    return false;
}

bool ShieldFilter::pointerButton(PointerButtonEvent *event)
{
    redirect(input()->pointer(), event->position);

    // The left and the right button have to reach the click target, an internal
    // window that only gets the press if no earlier filter takes it: one
    // activates or drags the thumbnail's window, the other opens its menu.
    if (event->button == Qt::LeftButton || event->button == Qt::RightButton) {
        return false;
    }

    // Every other button is the window's own, so it goes to the spot of the
    // window the thumbnail was pressed on rather than to the middle: a press is
    // at a place, and a thumbnail is a picture of the window that says which one.
    if (event->state == PointerButtonState::Pressed) {
        const Thumbnail *thumbnail = usableThumbnailAt(event->position);
        if (!thumbnail) {
            return false;
        }
        m_forwarder.enter(thumbnail->window, mapToWindow(*thumbnail, event->position));
        m_forwarder.button(event->button, event->state);
        if (m_clicked) {
            m_clicked(thumbnail->window);
        }
        return true;
    }

    // A release always follows its press into the same window, whatever has
    // become of the thumbnail in the meantime: a client that never sees the
    // button come up holds it down for good.
    if (!m_forwarder.hasButtons()) {
        return false;
    }
    m_forwarder.button(event->button, event->state);
    updateForwardedPointer(event->position, usableThumbnailAt(event->position));
    return true;
}

bool ShieldFilter::pointerAxis(PointerAxisEvent *event)
{
    redirect(input()->pointer(), event->position);

    const Thumbnail *thumbnail = usableThumbnailAt(event->position);
    if (!thumbnail) {
        return false;
    }

    // A scroll acts on the view rather than on a spot in it, so it is put into
    // the middle of the window. A motion event has normally set this up already;
    // a wheel turned without the pointer having moved gets it done here.
    m_forwarder.enter(thumbnail->window, InputForwarder::windowCentre(thumbnail->window));
    m_forwarder.axis(event);
    return true;
}

// The touchpad gestures act on the view as a whole too, so the target is settled
// once, when the fingers go down, and the rest of the gesture follows it. Only
// the gestures nothing else claimed reach this far: the ones bound to a desktop
// action are taken by the global shortcut filter long before.

bool ShieldFilter::swipeGestureBegin(PointerSwipeGestureBeginEvent *event)
{
    if (!beginGesture()) {
        return false;
    }
    m_forwarder.swipeBegin(event->fingerCount);
    return true;
}

bool ShieldFilter::swipeGestureUpdate(PointerSwipeGestureUpdateEvent *event)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.swipeUpdate(event->delta);
    return true;
}

bool ShieldFilter::swipeGestureEnd(PointerSwipeGestureEndEvent *)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.swipeEnd(false);
    return true;
}

bool ShieldFilter::swipeGestureCancelled(PointerSwipeGestureCancelEvent *)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.swipeEnd(true);
    return true;
}

bool ShieldFilter::pinchGestureBegin(PointerPinchGestureBeginEvent *event)
{
    if (!beginGesture()) {
        return false;
    }
    m_forwarder.pinchBegin(event->fingerCount);
    return true;
}

bool ShieldFilter::pinchGestureUpdate(PointerPinchGestureUpdateEvent *event)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.pinchUpdate(event->delta, event->scale, event->angleDelta);
    return true;
}

bool ShieldFilter::pinchGestureEnd(PointerPinchGestureEndEvent *)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.pinchEnd(false);
    return true;
}

bool ShieldFilter::pinchGestureCancelled(PointerPinchGestureCancelEvent *)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.pinchEnd(true);
    return true;
}

bool ShieldFilter::holdGestureBegin(PointerHoldGestureBeginEvent *event)
{
    if (!beginGesture()) {
        return false;
    }
    m_forwarder.holdBegin(event->fingerCount);
    return true;
}

bool ShieldFilter::holdGestureEnd(PointerHoldGestureEndEvent *)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.holdEnd(false);
    return true;
}

bool ShieldFilter::holdGestureCancelled(PointerHoldGestureCancelEvent *)
{
    if (!m_forwarder.hasGesture()) {
        return false;
    }
    m_forwarder.holdEnd(true);
    return true;
}

bool ShieldFilter::touchDown(TouchDownEvent *event)
{
    // Only the first point of a sequence can change the focus, the rest of it
    // belongs to whoever got that one; KWin blocks the update itself, so
    // redirecting on every point down is enough and never splits a sequence.
    redirect(input()->touch(), event->pos);

    const Thumbnail *thumbnail = usableThumbnailAt(event->pos);
    if (!thumbnail) {
        return false;
    }

    // The window already has the sequence, so it gets the rest of it too.
    if (m_forwarder.touchWindow() == thumbnail->window) {
        m_forwarder.touchDown(thumbnail->window, event->id, mapToWindow(*thumbnail, event->pos));
        return true;
    }

    // A single finger is the thumbnail's own: it taps to activate, holds for the
    // window menu, or drags the window out. A second one on the same thumbnail
    // is none of those, so the whole gesture is what the window is being asked
    // for, and both fingers are handed over at once: the click target is told to
    // forget the point it was following, and the point it never decided anything
    // about is put down in the window as if it had landed there. A drag that is
    // already carrying the window away is left to finish.
    const auto pending = std::find_if(m_pendingTouches.cbegin(), m_pendingTouches.cend(),
        [thumbnail](const PendingTouch &touch) { return touch.window == thumbnail->window; });
    if (pending == m_pendingTouches.cend() || workspace()->moveResizeWindow()) {
        m_pendingTouches.append(PendingTouch { event->id, event->pos, thumbnail->window });
        return false;
    }

    const PendingTouch first = *pending;
    m_pendingTouches.removeIf(
        [&first](const PendingTouch &touch) { return touch.id == first.id; });

    if (m_touchTakenOver) {
        m_touchTakenOver(thumbnail->window);
    }
    m_forwarder.touchDown(thumbnail->window, first.id, mapToWindow(*thumbnail, first.pos));
    m_forwarder.touchDown(thumbnail->window, event->id, mapToWindow(*thumbnail, event->pos));
    return true;
}

bool ShieldFilter::touchMotion(TouchMotionEvent *event)
{
    if (!m_forwarder.isTouchForwarded(event->id)) {
        // Not handed over (yet): the click target is following this one, and all
        // that is wanted here is where it has got to, in case a second finger
        // arrives and the pair has to be put down in the window.
        for (PendingTouch &touch : m_pendingTouches) {
            if (touch.id == event->id) {
                touch.pos = event->pos;
            }
        }
        return false;
    }

    // The window keeps the sequence even where the thumbnail no longer reaches:
    // a finger sliding off it is still part of the gesture the window is
    // following, and the point of the window it stands for is measured all the
    // same.
    const Thumbnail *thumbnail = thumbnailOf(m_forwarder.touchWindow());
    m_forwarder.touchMotion(event->id,
        thumbnail ? mapToWindow(*thumbnail, event->pos)
                  : InputForwarder::windowCentre(m_forwarder.touchWindow()));
    return true;
}

bool ShieldFilter::touchUp(TouchUpEvent *event)
{
    m_pendingTouches.removeIf([event](const PendingTouch &touch) { return touch.id == event->id; });

    if (!m_forwarder.isTouchForwarded(event->id)) {
        return false;
    }

    m_forwarder.touchUp(event->id);
    return true;
}

bool ShieldFilter::touchCancel()
{
    m_pendingTouches.clear();
    m_forwarder.touchCancel();
    return false;
}

// ---------------------------------------------------------------------------
// DragDropFilter
// ---------------------------------------------------------------------------

DragDropFilter::DragDropFilter(const ShieldFilter &shields)
    // Ahead of KWin's own drag and drop filter, which answers for every drag
    // there is and swallows the events whole: a thumbnail can only be offered
    // the drag before that one has decided the drop belongs to the click target.
    : InputEventFilter(InputFilterOrder::ScreenEdge)
    , m_shields(shields)
{
    m_dwellTimer.setSingleShot(true);
    QObject::connect(&m_dwellTimer, &QTimer::timeout, &m_dwellTimer, [this]() {
        SeatInterface *s = seat();
        if (!s || !s->isDrag() || !m_dwellWindow || !m_activate) {
            return;
        }

        // Once, and then the wait is over: activating ends the bloom, so the
        // thumbnail is gone and the drag carries on over the real window.
        m_activate(m_dwellWindow);
        m_dwellWindow.clear();
    });
}

void DragDropFilter::setActivationDelay(int msec) { m_delay = std::max(0, msec); }

void DragDropFilter::setActivationHandler(std::function<void(Window *)> handler)
{
    m_activate = std::move(handler);
}

void DragDropFilter::dwellOn(Window *window)
{
    // Resting on the same thumbnail is what the wait measures, so an unchanged
    // window leaves the timer running rather than starting it over.
    if (m_dwellWindow == window) {
        return;
    }

    m_dwellWindow = window;
    m_dwellTimer.stop();
    if (window && m_delay > 0) {
        m_dwellTimer.start(m_delay);
    }
}

bool DragDropFilter::dragOnto(const QPointF &pos, bool movePointer)
{
    SeatInterface *s = seat();
    if (!s || !s->isDrag()) {
        dwellOn(nullptr);
        return false;
    }

    const ShieldFilter::Thumbnail *thumbnail = m_shields.usableThumbnailAt(pos);
    SurfaceInterface *surface = thumbnail ? thumbnail->window->surface() : nullptr;
    if (!surface) {
        // Not on a thumbnail: KWin's own filter takes the drag from here, and
        // whatever it points at is the right target.
        dwellOn(nullptr);
        return false;
    }

    stampSeat();

    // The pointer itself still goes where it really is: the drag icon is drawn
    // under it, and one left behind would come away from the finger holding it.
    if (movePointer) {
        s->notifyPointerMotion(pos);
        s->notifyPointerFrame();
    }

    // The drop, though, is aimed at the middle of the window. The window is
    // somewhere else entirely and nothing about the thumbnail says where inside
    // it a drop belongs, so it is offered to the view as a whole, exactly like a
    // scroll. Retargeting is what sends the leave and the enter, so it is done
    // only when the drag actually changes windows; the motion afterwards is what
    // keeps the client's own feedback alive, and it comes last so that the
    // centre is the position that stands.
    if (surface != s->dragSurface()) {
        s->setDragTarget(s->dropHandlerForSurface(surface), surface,
            InputForwarder::windowCentre(thumbnail->window), thumbnail->window->inputTransformation());
    }
    s->notifyDragMotion(InputForwarder::windowCentre(thumbnail->window));

    dwellOn(thumbnail->window);
    return true;
}

bool DragDropFilter::pointerMotion(PointerMotionEvent *event)
{
    return dragOnto(event->position, true);
}

bool DragDropFilter::touchMotion(TouchMotionEvent *event)
{
    // A touch drag carries no pointer, so there is nothing to move along.
    return seat() && seat()->isDragTouch() ? dragOnto(event->pos, false) : false;
}

TouchDragFilter::TouchDragFilter()
    : InputEventFilter(InputFilterOrder::Effects)
{ }

void TouchDragFilter::arm(Window *window, qint32 id)
{
    m_window = window;
    m_id = id;
}

void TouchDragFilter::disarm(bool cancel)
{
    if (m_window && workspace()->moveResizeWindow() == m_window) {
        if (cancel) {
            m_window->cancelInteractiveMoveResize();
        } else {
            m_window->endInteractiveMoveResize();
        }
    }
    m_window.clear();
    m_id = -1;
}

bool TouchDragFilter::touchMotion(TouchMotionEvent *event)
{
    if (!m_window || event->id != m_id) {
        return false;
    }

    // Anything else that ends the move (a shortcut, the window closing) leaves
    // the sequence to whoever else wants it.
    if (workspace()->moveResizeWindow() != m_window) {
        disarm(false);
        return false;
    }

    m_window->updateInteractiveMoveResize(event->pos, input()->keyboardModifiers());
    return true;
}

bool TouchDragFilter::touchUp(TouchUpEvent *event)
{
    if (!m_window || event->id != m_id) {
        return false;
    }

    disarm(false);
    return true;
}

bool TouchDragFilter::touchCancel()
{
    if (!m_window) {
        return false;
    }

    disarm(true);
    return true;
}

} // namespace ThumbnailBloom
