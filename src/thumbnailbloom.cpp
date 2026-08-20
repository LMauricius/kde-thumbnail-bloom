/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailbloom.h"
#include "thumbnailbloomconfig.h"
#include "thumbnailoverlay.h"

#include <core/output.h>
#include <cursor.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>

#include <QHash>
#include <QSet>
#include <QVector2D>

#include <algorithm>
#include <vector>

using namespace KWin;

namespace ThumbnailBloom
{

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/*! Returns the rectangle \a progress of the way from \a from to \a to. */
static QRectF interpolateRect(const QRectF &from, const QRectF &to, qreal progress)
{
    const qreal inverse = 1.0 - progress;
    return QRectF(from.x() * inverse + to.x() * progress,
                  from.y() * inverse + to.y() * progress,
                  from.width() * inverse + to.width() * progress,
                  from.height() * inverse + to.height() * progress);
}

/*!
 * Returns \a rect grown around its centre halfway towards the size of \a natural,
 * pushed inside \a area.
 *
 * Growing around the centre (rather than towards the real window) keeps the
 * result a superset of \a rect, so the pointer cannot end up outside the grown
 * thumbnail while still being inside the resting one, which would make the
 * thumbnail flip between the two sizes forever.
 */
static QRectF grownRect(const QRectF &rect, const QRectF &natural, const QRectF &area)
{
    const QSizeF size(std::max(rect.width(), (rect.width() + natural.width()) / 2),
                      std::max(rect.height(), (rect.height() + natural.height()) / 2));
    QRectF grown(QPointF(rect.center().x() - size.width() / 2, rect.center().y() - size.height() / 2), size);

    // Only shift the result: resizing it again would break the superset property.
    grown.moveLeft(std::min(std::max(grown.left(), area.left()), std::max(area.right() - grown.width(), area.left())));
    grown.moveTop(std::min(std::max(grown.top(), area.top()), std::max(area.bottom() - grown.height(), area.top())));
    return grown;
}

/*!
 * Returns the screen area a thumbnail of \a w resting at \a rect paints into.
 *
 * The window is drawn with its shadow, which reaches past the frame geometry the
 * layout works with, so everything that has to be repainted for the thumbnail is
 * larger than the thumbnail itself.
 */
static QRectF thumbnailBounds(EffectWindow *w, const QRectF &rect)
{
    const QRectF natural = w->frameGeometry();
    if (natural.isEmpty()) {
        return rect;
    }

    const qreal scaleX = rect.width() / natural.width();
    const qreal scaleY = rect.height() / natural.height();
    const QRectF expanded = w->expandedGeometry();
    return QRectF(rect.x() + (expanded.x() - natural.x()) * scaleX,
                  rect.y() + (expanded.y() - natural.y()) * scaleY,
                  expanded.width() * scaleX,
                  expanded.height() * scaleY);
}

/*! Returns whether \a a and \a b are the same rectangle for painting purposes. */
static bool sameRect(const QRectF &a, const QRectF &b)
{
    constexpr qreal epsilon = 0.01;
    return std::abs(a.x() - b.x()) < epsilon && std::abs(a.y() - b.y()) < epsilon
        && std::abs(a.width() - b.width()) < epsilon && std::abs(a.height() - b.height()) < epsilon;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

ThumbnailBloomEffect::ThumbnailBloomEffect()
    : m_animationDuration(250)
{
    // Changes tend to arrive in bursts (a raise is a stacking change plus an
    // activation plus a geometry change), so they only mark the layout dirty.
    m_relayoutTimer.setSingleShot(true);
    m_relayoutTimer.setInterval(0);
    connect(&m_relayoutTimer, &QTimer::timeout, this, &ThumbnailBloomEffect::relayout);

    connect(effects, &EffectsHandler::windowAdded, this, [this](EffectWindow *w) {
        watch(w);
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::windowClosed, this, [this](EffectWindow *w) {
        forget(w);
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::windowDeleted, this, [this](EffectWindow *w) {
        forget(w);
    });
    connect(effects, &EffectsHandler::windowActivated, this, [this](EffectWindow *) {
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::stackingOrderChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(effects, &EffectsHandler::currentActivityChanged, this, [this](const QString &) {
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::desktopChanged, this, [this](VirtualDesktop *, VirtualDesktop *, EffectWindow *, LogicalOutput *) {
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::screenAdded, this, [this](LogicalOutput *) {
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::screenRemoved, this, [this](LogicalOutput *) {
        scheduleRelayout();
    });

    // Hover is tracked from the cursor position rather than from the overlays:
    // KWin dispatches pointer events to internal windows on its own and never
    // synthesises the enter and leave events a QWindow would otherwise receive.
    connect(Cursors::self(), &Cursors::positionChanged, this, [this](Cursor *, const QPointF &pos) {
        updateHover(pos);
    });

    for (EffectWindow *w : effects->stackingOrder()) {
        watch(w);
    }

    reconfigure(ReconfigureAll);
}

ThumbnailBloomEffect::~ThumbnailBloomEffect() = default;

void ThumbnailBloomEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)

    ThumbnailBloomConfig::self()->read();

    m_skipKeepAbove = ThumbnailBloomConfig::skipKeepAbove();
    m_skipMaximized = ThumbnailBloomConfig::skipMaximized();
    m_skipParents = ThumbnailBloomConfig::skipParents();
    m_skipChildren = ThumbnailBloomConfig::skipChildren();

    m_layoutOptions.initialScale = std::clamp(ThumbnailBloomConfig::initialSize() / 100.0, 0.1, 1.0);
    m_layoutOptions.minScale = std::clamp(ThumbnailBloomConfig::minimumSize() / 100.0, 0.05, m_layoutOptions.initialScale);
    m_layoutOptions.scaleStep = 0.05;
    m_layoutOptions.margin = 8;

    // The system's animation speed is already folded into animationTime().
    m_animationDuration = std::max(std::chrono::milliseconds(1), animationTime(std::chrono::milliseconds(250)));

    scheduleRelayout();
}

void ThumbnailBloomEffect::watch(EffectWindow *w)
{
    // Anything that can change what covers what invalidates the layout.
    connect(w, &EffectWindow::windowFrameGeometryChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowMaximizedStateChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowFullScreenChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowKeepAboveChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::minimizedChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowDesktopsChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowHiddenChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowModalityChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowStartUserMovedResized, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowFinishUserMovedResized, this, &ThumbnailBloomEffect::scheduleRelayout);

    // A thumbnail is painted away from the window it belongs to, so the damage
    // KWin schedules for the window itself does not cover it.
    connect(w, &EffectWindow::windowDamaged, this, [this](EffectWindow *window) {
        const auto it = m_states.find(window);
        if (it != m_states.end()) {
            effects->addRepaint(RectF(thumbnailBounds(window, it->second.current).adjusted(-1, -1, 1, 1)));
        }
    });
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ThumbnailBloomEffect::scheduleRelayout()
{
    m_relayoutTimer.start();
}

void ThumbnailBloomEffect::relayout()
{
    const QList<EffectWindow *> stack = effects->stackingOrder();

    // Windows something else is transient for, needed by the "skip parents" setting.
    QSet<EffectWindow *> parents;
    for (EffectWindow *w : stack) {
        const QList<EffectWindow *> mainWindows = w->mainWindows();
        for (EffectWindow *parent : mainWindows) {
            parents.insert(parent);
        }
    }

    // Windows only ever collide with windows of their own screen, so each screen
    // is laid out on its own. The stacking order is preserved per screen.
    QHash<LogicalOutput *, QList<LayoutWindow>> perScreen;
    for (EffectWindow *w : stack) {
        if (!isRelevant(w)) {
            continue;
        }
        perScreen[w->screen()].append(LayoutWindow{w, QRectF(w->frameGeometry()), isEligible(w, parents)});
    }

    QSet<EffectWindow *> bloomed;
    for (auto it = perScreen.cbegin(); it != perScreen.cend(); ++it) {
        const QRectF workArea = effects->clientArea(MaximizeArea, it.key());
        for (const Placement &placement : computeLayout(it.value(), workArea, m_layoutOptions)) {
            EffectWindow *w = static_cast<EffectWindow *>(placement.id);
            bloomed.insert(w);
            retarget(w, placement.rect);
        }
    }

    // Windows that stopped blooming travel back to where they really are.
    for (auto &[w, state] : m_states) {
        if (!bloomed.contains(w)) {
            state.hovered = false;
            retarget(w, QRectF(w->frameGeometry()));
        }
    }
}

void ThumbnailBloomEffect::retarget(EffectWindow *w, const QRectF &base)
{
    const auto [it, inserted] = m_states.try_emplace(w);
    BloomState &state = it->second;

    state.base = base;
    const QRectF target = state.hovered
        ? grownRect(base, QRectF(w->frameGeometry()), QRectF(effects->clientArea(MaximizeArea, w)))
        : base;

    if (inserted) {
        state.from = state.current = state.to = QRectF(w->frameGeometry());
        state.timeline.setEasingCurve(QEasingCurve::InOutCubic);
    } else if (sameRect(state.to, target)) {
        return;
    }

    state.from = state.current;
    state.to = target;
    state.timeline.setDuration(m_animationDuration);
    state.timeline.reset();

    // The click target only makes sense once the thumbnail has come to rest.
    if (state.overlay) {
        state.overlay->hide();
    }

    m_animating = true;
    effects->addRepaintFull();
}

void ThumbnailBloomEffect::updateHover(const QPointF &pos)
{
    // While a thumbnail animates it moves under a standing pointer and its click
    // target is hidden, so only resting thumbnails take part; the hover is
    // evaluated again as soon as one comes to rest.
    const auto settled = [](const BloomState &state) {
        return state.timeline.done() && state.overlay && state.overlay->isVisible();
    };

    // The hit test is against the resting rectangle, never the grown one: the
    // pointer keeps the thumbnail enlarged only while it stays inside the area
    // the thumbnail occupies when it is not hovered. Testing the grown rectangle
    // instead would make the thumbnail hold on to the pointer over an area it
    // only covers because of that very pointer.
    // At most one thumbnail is hovered, and the hovered one keeps it as long as
    // the pointer stays on it.
    EffectWindow *hovered = nullptr;
    for (const auto &[w, state] : m_states) {
        if (settled(state) && state.base.contains(pos) && (!hovered || state.hovered)) {
            hovered = w;
        }
    }

    for (const auto &[w, state] : m_states) {
        if (settled(state)) {
            setHovered(w, w == hovered);
        }
    }
}

void ThumbnailBloomEffect::setHovered(EffectWindow *w, bool hovered)
{
    const auto it = m_states.find(w);
    if (it == m_states.end() || it->second.hovered == hovered) {
        return;
    }

    it->second.hovered = hovered;

    // The lift outlives the hover: a thumbnail that is shrinking back has to
    // stay above the windows it grew over until it has arrived.
    if (hovered) {
        m_liftedWindow = w;
    }
    if (it->second.overlay) {
        it->second.overlay->setHovered(hovered);
    }

    // Never retarget from here. This runs either inside KWin's pointer dispatch
    // (through the cursor position signal) or inside the paint pass, and
    // retargeting hides the overlay: hiding an internal window makes KWin destroy
    // it right away, under the very code that is still using it. The relayout
    // timer moves that to a safe point of the event loop instead.
    scheduleRelayout();
}

void ThumbnailBloomEffect::forget(EffectWindow *w)
{
    if (m_liftedWindow == w) {
        m_liftedWindow = nullptr;
    }
    m_states.erase(w);
    effects->addRepaintFull();
}

void ThumbnailBloomEffect::updateOverlay(EffectWindow *w, BloomState &state)
{
    if (!state.overlay) {
        state.overlay = std::make_unique<ThumbnailOverlay>();
        connect(state.overlay.get(), &ThumbnailOverlay::clicked, this, [w]() {
            effects->activateWindow(w);
        });
    }

    const bool wasVisible = state.overlay->isVisible();
    const QRect geometry = state.current.toAlignedRect();
    state.overlay->setHovered(state.hovered);
    state.overlay->setGeometry(geometry);
    state.overlay->show();

    // The cursor does not have to move for a thumbnail to come to rest under it,
    // so the hover is evaluated once more whenever a click target reappears.
    if (!wasVisible) {
        updateHover(effects->cursorPos());
    }
}

// ---------------------------------------------------------------------------
// Window classification
// ---------------------------------------------------------------------------

bool ThumbnailBloomEffect::isRelevant(EffectWindow *w) const
{
    if (w->isDeleted() || w->isMinimized() || w->isHidden() || !w->screen()) {
        return false;
    }
    if (!w->isOnCurrentDesktop() || !w->isOnCurrentActivity()) {
        return false;
    }
    if (w->internalWindow() || w->isDesktop() || w->isDock() || w->isPopupWindow() || w->isPopupMenu()) {
        return false;
    }
    if (w->isSplash() || w->isUtility() || w->isToolbar() || w->isOutline() || w->isLockScreen()) {
        return false;
    }

    return w->isNormalWindow() || w->isDialog();
}

bool ThumbnailBloomEffect::isEligible(EffectWindow *w, const QSet<EffectWindow *> &parents) const
{
    if (w == effects->activeWindow() || w->isUserMove() || w->isUserResize()) {
        return false;
    }
    if (m_skipKeepAbove && w->keepAbove()) {
        return false;
    }
    if (m_skipMaximized && isMaximized(w)) {
        return false;
    }
    if (m_skipChildren && w->transientFor()) {
        return false;
    }
    if (m_skipParents && parents.contains(w)) {
        return false;
    }

    return true;
}

bool ThumbnailBloomEffect::isMaximized(EffectWindow *w) const
{
    if (w->isFullScreen()) {
        return true;
    }

    const QRectF area = effects->clientArea(MaximizeArea, w);
    const QRectF geometry = w->frameGeometry();
    return geometry.width() >= area.width() - 1 && geometry.height() >= area.height() - 1;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void ThumbnailBloomEffect::applyTransform(EffectWindow *w, const BloomState &state, WindowPaintData &data) const
{
    const QRectF natural = w->frameGeometry();
    if (natural.width() <= 0 || natural.height() <= 0) {
        return;
    }

    // Scaling happens around the window's top left corner, so the translation is
    // expressed in unscaled screen coordinates.
    data.setScale(QVector2D(state.current.width() / natural.width(), state.current.height() / natural.height()));
    data.setXTranslation(state.current.x() - natural.x());
    data.setYTranslation(state.current.y() - natural.y());
}

void ThumbnailBloomEffect::prePaintScreen(ScreenPrePaintData &data)
{
    m_animating = false;

    std::vector<EffectWindow *> settledBack;
    for (auto &[w, state] : m_states) {
        state.timeline.advance(data.view);
        state.current = interpolateRect(state.from, state.to, state.timeline.value());

        if (!state.timeline.done()) {
            m_animating = true;
            continue;
        }

        // A thumbnail that has arrived back at its window is not a thumbnail
        // any more; everything else gets its click target placed.
        if (sameRect(state.to, QRectF(w->frameGeometry()))) {
            settledBack.push_back(w);
        } else {
            updateOverlay(w, state);
        }
    }

    // The lift ends only once the thumbnail has come to rest again.
    if (m_liftedWindow) {
        const auto it = m_states.find(m_liftedWindow);
        if (it == m_states.end() || (!it->second.hovered && it->second.timeline.done())) {
            m_liftedWindow = nullptr;
        }
    }

    // The lifted thumbnail is drawn right after the topmost window that covers
    // it, and not after the whole screen: everything painted later (the popup
    // layer the outlines live in, and the cursor) keeps painting over it.
    //
    // Anchoring to a window the thumbnail overlaps also keeps the two in the
    // same repaint: a window that does not intersect the thumbnail may be left
    // out of a partial repaint, and the anchor has to be painted for the
    // thumbnail to be drawn at all.
    m_liftAnchor = nullptr;
    if (m_liftedWindow) {
        const QRectF thumbnail = m_states.at(m_liftedWindow).current;

        bool above = false;
        for (EffectWindow *w : effects->stackingOrder()) {
            if (w == m_liftedWindow) {
                // The stacking order runs bottom to top, so only what follows
                // covers the thumbnail; anything below it is already covered.
                above = true;
            } else if (above && isRelevant(w) && w->frameGeometry().toRect().intersects(Rect(thumbnail.toAlignedRect()))) {
                m_liftAnchor = w;
            }
        }
    }
    m_liftPending = m_liftAnchor != nullptr;

    for (EffectWindow *w : settledBack) {
        if (m_liftedWindow == w) {
            m_liftedWindow = nullptr;
        }
        m_states.erase(w);
    }

    Effect::prePaintScreen(data);
}

void ThumbnailBloomEffect::prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data)
{
    const auto it = m_states.find(w);
    if (it != m_states.end() && !sameRect(it->second.current, QRectF(w->frameGeometry()))) {
        // The window is painted somewhere else and at another size, so it may
        // neither be clipped against nor culled by its real geometry.
        data.setTransformed();
        data.setTranslucent();
    }

    Effect::prePaintWindow(view, w, data);
}

void ThumbnailBloomEffect::paintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    // The lifted thumbnail is left out at its own place in the stacking order and
    // drawn again after the anchor: not chaining the call is what keeps a window
    // out of a pass. Without an anchor it is already the topmost window and can
    // simply be painted where it is.
    if (w == m_liftedWindow && m_liftPending) {
        return;
    }

    const auto it = m_states.find(w);
    if (it != m_states.end()) {
        applyTransform(w, it->second, data);
    }

    Effect::paintWindow(renderTarget, viewport, w, mask, deviceRegion, data);

    if (w == m_liftAnchor) {
        drawLifted(renderTarget, viewport);
    }
}

void ThumbnailBloomEffect::drawLifted(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    if (!m_liftPending) {
        return;
    }
    m_liftPending = false;

    const auto it = m_states.find(m_liftedWindow);
    if (it == m_states.end()) {
        return;
    }

    // The region is the clip of the draw, and it has to be the damage of the
    // whole pass. The region the anchor was painted with is clipped to that
    // window's own area for an opaque window, which cuts the thumbnail down to
    // the part overlapping it; an infinite region has the opposite problem, since
    // painting outside the damage blends the translucent parts of the thumbnail
    // over pixels that were never cleared, so the shadow darkens frame by frame.
    WindowPaintData data;
    applyTransform(m_liftedWindow, it->second, data);
    effects->drawWindow(renderTarget, viewport, m_liftedWindow,
                        PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT, m_paintRegion, data);
}

void ThumbnailBloomEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    // Nothing is painted here; this only keeps hold of the damage of the pass for
    // the thumbnail that is stamped from inside the window pass, which is handed
    // a per-window region instead.
    m_paintRegion = deviceRegion;

    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
}

void ThumbnailBloomEffect::postPaintScreen()
{
    // A lifted thumbnail is drawn out of turn, right after the anchor, so the two
    // have to be painted in the same pass. Partial damage does not guarantee
    // that: a repaint of the area the pointer moved through paints the windows
    // below the thumbnail without ever reaching the anchor, and the thumbnail is
    // erased wherever that happens. Repainting everything, exactly as the
    // animations do, is what keeps every thumbnail whole.
    if (m_animating || m_liftedWindow) {
        effects->addRepaintFull();
    }

    Effect::postPaintScreen();
}

bool ThumbnailBloomEffect::isActive() const
{
    return !m_states.empty();
}

int ThumbnailBloomEffect::requestedEffectChainPosition() const
{
    return 50;
}

} // namespace ThumbnailBloom

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(ThumbnailBloom::ThumbnailBloomEffect, "metadata.json", return true;, return false;)

#include "thumbnailbloom.moc"
