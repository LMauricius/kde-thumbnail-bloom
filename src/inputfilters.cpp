/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inputfilters.h"

#include <input_event.h>
#include <pointer_input.h>
#include <touch_input.h>
#include <window.h>
#include <workspace.h>

#include <algorithm>

using namespace KWin;

namespace ThumbnailBloom {

ShieldFilter::ShieldFilter()
    : InputEventFilter(InputFilterOrder::Popup)
{ }

void ShieldFilter::setState(
    const QRegion &shields, const QSet<Window *> &bloomed, const QList<Thumbnail> &thumbnails)
{
    m_shields = shields;
    m_bloomed = bloomed;
    m_thumbnails = thumbnails;
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

bool ShieldFilter::isThumbnailUsable(const QPointF &pos) const
{
    // The pixels a window is painted over belong to that window, so a thumbnail
    // does nothing there, however much of its click target reaches over it.
    const Thumbnail *thumbnail = thumbnailAt(pos);
    return thumbnail && !isCovered(thumbnail->window, pos);
}

void ShieldFilter::redirect(InputDeviceHandler *device, const QPointF &pos)
{
    // Only what one of ours caught is moved on: a shield, or a click target on a
    // part of its thumbnail that a window is painted over. Anything else is
    // already focused where it belongs.
    Window *focus = device->focus();
    if (!focus || !focus->isInternal()) {
        return;
    }

    // One walk of the stack answers both halves, which matters on a path every
    // pointer motion takes: what is really under the pointer, and whether it is
    // stacked above the thumbnail the click target belongs to, which is what
    // makes that thumbnail invisible here.
    const Thumbnail *thumbnail = thumbnailAt(pos);
    const Hit hit = hitAt(pos, thumbnail ? thumbnail->window : nullptr);
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
    redirect(input()->pointer(), event->position);
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
    return isThumbnailUsable(event->position);
}

bool ShieldFilter::pointerAxis(PointerAxisEvent *event)
{
    redirect(input()->pointer(), event->position);
    return isThumbnailUsable(event->position);
}

bool ShieldFilter::touchDown(TouchDownEvent *event)
{
    // Only the first point of a sequence can change the focus, the rest of it
    // belongs to whoever got that one; KWin blocks the update itself, so
    // redirecting on every point down is enough and never splits a sequence.
    redirect(input()->touch(), event->pos);
    return false;
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
