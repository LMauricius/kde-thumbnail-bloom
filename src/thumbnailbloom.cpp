/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailbloom.h"
#include "thumbnailbloomconfig.h"
#include "thumbnailoverlay.h"

#include <core/colorspace.h>
#include <core/output.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <cursor.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <opengl/glutils.h>

#include <KColorScheme>

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

//! Width of the hover outline, in logical pixels.
constexpr qreal outlineWidth = 2.0;

/*! Returns the two triangles covering \a rect, appended to \a vertices. */
static void appendQuad(std::vector<QVector2D> &vertices, const QRectF &rect)
{
    const QVector2D topLeft(rect.left(), rect.top());
    const QVector2D topRight(rect.right(), rect.top());
    const QVector2D bottomLeft(rect.left(), rect.bottom());
    const QVector2D bottomRight(rect.right(), rect.bottom());
    vertices.insert(vertices.end(), {topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight});
}

/*! Returns the outline colour of the current colour scheme. */
static QColor outlineColor()
{
    // Read on every use: the colour scheme can change while the effect runs.
    return KColorScheme(QPalette::Active, KColorScheme::View).decoration(KColorScheme::FocusColor).color();
}

/*!
 * Returns whether \a w is a system element: something KWin paints in a layer of
 * its own above the ordinary windows (panels, popups, applet popups, menus,
 * notifications, on screen displays) and that never takes part in the layout, so
 * a thumbnail can end up underneath it.
 */
static bool isSystemElement(EffectWindow *w)
{
    if (w->isDeleted() || !w->isVisible() || w->isMinimized() || w->isHidden()) {
        return false;
    }
    if (!w->isOnCurrentDesktop() || !w->isOnCurrentActivity()) {
        return false;
    }

    // Internal windows are KWin's own surfaces (its on screen displays and the
    // like); the effect's click targets are internal too and are filtered out by
    // the caller, which is the only place that can tell them apart.
    return w->internalWindow() || w->isDock() || w->isPopupWindow() || w->isPopupMenu() || w->isDropdownMenu()
        || w->isMenu() || w->isAppletPopup() || w->isNotification() || w->isCriticalNotification()
        || w->isOnScreenDisplay() || w->isTooltip() || w->isComboBox() || w->isDNDIcon() || w->isSplash()
        || w->isLockScreen();
}

/*!
 * Returns whether \a w can take pointer input where it really is.
 *
 * Everything mapped on the current desktop qualifies, docks and popups included:
 * this is used to work out what a shield must not cover, so it has to err on the
 * side of leaving input alone.
 */
static bool isInputTarget(EffectWindow *w)
{
    if (w->isDeleted() || !w->isVisible() || w->isMinimized() || w->isHidden()) {
        return false;
    }

    return w->isOnCurrentDesktop() && w->isOnCurrentActivity();
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

ThumbnailBloomEffect::~ThumbnailBloomEffect()
{
    // Destroying an overlay makes KWin drop its internal window and emit
    // windowClosed synchronously, and that handler walks m_states: every
    // handler touching the map must be gone before the map is destructed, or
    // forget() re-enters a container that is going away.
    disconnect(effects, nullptr, this, nullptr);
    disconnect(Cursors::self(), nullptr, this, nullptr);
    for (auto &entry : m_states) {
        disconnect(entry.first, nullptr, this, nullptr);
    }
    const auto states = std::move(m_states);
}

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

    // Placing the click targets needs to know what covers them, so this comes
    // first.
    updateSystemRegion();

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

    // Needs the final click targets of every thumbnail, so it comes after the
    // whole layout rather than per window.
    updateShields();

    // The click targets have just been placed and moved, so the pointer can end
    // up on another thumbnail without having moved at all.
    updateHover(effects->cursorPos());
}

void ThumbnailBloomEffect::retarget(EffectWindow *w, const QRectF &base)
{
    const auto [it, inserted] = m_states.try_emplace(w);
    BloomState &state = it->second;

    state.base = base;
    const QRectF target = state.hovered
        ? grownRect(base, QRectF(w->frameGeometry()), QRectF(effects->clientArea(MaximizeArea, w)))
        : base;

    // The click target follows the resting rectangle, not the animation: a
    // thumbnail can be hovered and clicked from the moment it sets off, but only
    // where it is going to end up.
    updateOverlay(w, state);

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

    m_animating = true;
    effects->addRepaintFull();
}

void ThumbnailBloomEffect::updateHover(const QPointF &pos)
{
    // Everything with a placed click target takes part, animating or not: the
    // click target sits on the destination of the thumbnail, so the hit test
    // never follows it along its path.
    const auto targetable = [](const BloomState &state) {
        return state.overlay && state.overlay->isVisible();
    };

    // The hit test is against the exposed part of the resting rectangle, never
    // the grown one: the pointer keeps the thumbnail enlarged only while it
    // stays inside the area the thumbnail occupies when it is not hovered.
    // Testing the grown rectangle instead would make the thumbnail hold on to
    // the pointer over an area it only covers because of that very pointer.
    // What a panel or a popup covers belongs to that panel or popup, so it is
    // cut out of the region and hovering there does nothing.
    // At most one thumbnail is hovered, and the hovered one keeps it as long as
    // the pointer stays on it.
    EffectWindow *hovered = nullptr;
    for (const auto &[w, state] : m_states) {
        if (targetable(state) && state.hitRegion.contains(pos.toPoint()) && (!hovered || state.hovered)) {
            hovered = w;
        }
    }

    for (const auto &[w, state] : m_states) {
        if (targetable(state)) {
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

    // Extracted rather than erased in place: destroying the overlay makes KWin
    // emit windowClosed for its internal window synchronously, and that handler
    // calls back into m_states, which has to be consistent by then. The overlay
    // itself only dies on the next event loop pass, because this can run under
    // input dispatch, where destroying an internal window is not survivable;
    // its signals are cut right away so a click in the meantime cannot reach
    // the window pointer that is about to go stale.
    auto node = m_states.extract(w);
    if (!node.empty()) {
        for (OverlayWindow *window : {static_cast<OverlayWindow *>(node.mapped().overlay.release()),
                                      node.mapped().shield.release()}) {
            if (window) {
                window->disconnect();
                window->deleteLater();
            }
        }
    }
    effects->addRepaintFull();
}

void ThumbnailBloomEffect::updateOverlay(EffectWindow *w, BloomState &state)
{
    // Only ever reached from the relayout pass: hiding an internal window makes
    // KWin destroy it synchronously, which must not happen under pointer
    // dispatch or under the effect chain.
    //
    // A window travelling back to its own geometry stops being a thumbnail, so
    // it loses its click target right away rather than at the end of the trip.
    // The click target only claims what is actually visible of the thumbnail.
    // KWin hit tests an internal window against the mask of its QWindow, so
    // cutting the system elements out of that mask hands their own area back to
    // them: the panel keeps its hover feedback and its clicks, and so does every
    // popup that opens over a thumbnail.
    const QRect rect = state.base.toAlignedRect();
    state.hitRegion = QRegion(rect) - m_systemRegion;

    if (sameRect(state.base, QRectF(w->frameGeometry())) || state.hitRegion.isEmpty()) {
        state.hitRegion = QRegion();
        if (state.overlay) {
            state.overlay->hide();
        }
        return;
    }

    if (!state.overlay) {
        state.overlay = std::make_unique<ThumbnailOverlay>();
        connect(state.overlay.get(), &ThumbnailOverlay::clicked, this, [w]() {
            effects->activateWindow(w);
        });
    }

    // The resting rectangle, never the current one: the click target must not
    // travel with the animation, and never grows with the hover either.
    state.overlay->setGeometry(rect);
    state.overlay->setMask(state.hitRegion.translated(-rect.topLeft()));
    state.overlay->show();
}

void ThumbnailBloomEffect::updateShields()
{
    // Only ever reached from the relayout pass: showing and hiding internal
    // windows is not survivable under pointer dispatch or under the effect chain.
    //
    // A bloomed window is painted somewhere else but keeps its real input
    // geometry, so hovering or clicking the area it vacated would still reach it.
    // A shield is an internal window put on that area: KWin hit tests internal
    // windows above the ordinary ones, so the pointer focus lands on the shield
    // and the window below never sees an enter event either. Only clicks on the
    // thumbnail activate a bloomed window.

    // Whatever the thumbnails claim stays theirs; the shields are internal
    // windows too, and two of those on the same pixel have no defined order.
    QRegion thumbnails;
    for (const auto &[w, state] : m_states) {
        thumbnails += state.hitRegion;
    }

    // Walking the stack top down keeps a shield inside the area where its window
    // really is the topmost input target: everything above it has been added to
    // `covered` by the time the window is reached. Covering a window that lies
    // over a bloomed one would take away input that rightfully belongs to it.
    QRegion covered = m_systemRegion;
    QSet<EffectWindow *> shielded;
    const QList<EffectWindow *> stack = effects->stackingOrder();
    for (auto it = stack.crbegin(); it != stack.crend(); ++it) {
        EffectWindow *w = *it;
        if (!isInputTarget(w) || isOwnOverlay(w)) {
            continue;
        }

        const QRect frame = w->frameGeometry().toAlignedRect();
        const auto sit = m_states.find(w);

        // A non empty hit region is exactly what marks a window as bloomed: it is
        // set by updateOverlay() and cleared as soon as the window travels home.
        if (sit != m_states.end() && !sit->second.hitRegion.isEmpty()) {
            BloomState &state = sit->second;
            const QRegion exposed = QRegion(frame) - covered - thumbnails;
            if (!exposed.isEmpty()) {
                if (!state.shield) {
                    state.shield = std::make_unique<OverlayWindow>();
                }
                const QRect bounds = exposed.boundingRect();
                state.shield->setGeometry(bounds);
                state.shield->setMask(exposed.translated(-bounds.topLeft()));
                state.shield->show();
                shielded.insert(w);
            }
        }

        // The bloomed window takes part as well: it is shielded where it is
        // exposed, so a window below must not claim that area either.
        covered += frame;
    }

    // Everything else drops its shield, the windows the loop never reached
    // included: a window that got hidden or unbloomed must take its own input
    // back immediately.
    for (auto &[w, state] : m_states) {
        if (state.shield && !shielded.contains(w)) {
            state.shield->hide();
        }
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

bool ThumbnailBloomEffect::isOwnOverlay(EffectWindow *w) const
{
    const QWindow *handle = w->internalWindow();
    if (!handle) {
        return false;
    }

    return std::any_of(m_states.begin(), m_states.end(), [handle](const auto &entry) {
        return entry.second.overlay.get() == handle || entry.second.shield.get() == handle;
    });
}

void ThumbnailBloomEffect::updateSystemRegion()
{
    m_systemRegion = QRegion();
    for (EffectWindow *w : effects->stackingOrder()) {
        // Stacking is not consulted: system elements live in layers above the
        // ordinary windows, and a thumbnail is painted in the layer of the
        // window it belongs to, so one always covers the other.
        if (isSystemElement(w) && !isOwnOverlay(w)) {
            m_systemRegion += w->frameGeometry().toAlignedRect();
        }
    }
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
        // any more. Click targets are placed by the relayout, not from here.
        if (sameRect(state.to, QRectF(w->frameGeometry()))) {
            settledBack.push_back(w);
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
    } else if (w == m_liftedWindow && it != m_states.end()) {
        // Nothing covers this thumbnail, so it was painted in place just now and
        // only the outline is left to put on top of it.
        drawOutline(renderTarget, viewport, it->second.current);
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

    drawOutline(renderTarget, viewport, it->second.current);
}

void ThumbnailBloomEffect::drawOutline(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRectF &rect) const
{
    if (!effects->isOpenGLCompositing() || rect.isEmpty()) {
        return;
    }

    // Four quads laid inside the edges of the thumbnail, in logical screen
    // coordinates: that is what the projection matrix of the viewport expects.
    const QRectF inner = rect.adjusted(outlineWidth, outlineWidth, -outlineWidth, -outlineWidth);
    std::vector<QVector2D> vertices;
    vertices.reserve(24);
    appendQuad(vertices, QRectF(rect.left(), rect.top(), rect.width(), outlineWidth));
    appendQuad(vertices, QRectF(rect.left(), inner.bottom(), rect.width(), outlineWidth));
    appendQuad(vertices, QRectF(rect.left(), inner.top(), outlineWidth, inner.height()));
    appendQuad(vertices, QRectF(inner.right(), inner.top(), outlineWidth, inner.height()));

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setVertices(vertices);

    ShaderBinder binder(ShaderTrait::UniformColor | ShaderTrait::TransformColorspace);
    GLShader *shader = binder.shader();
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, viewport.projectionMatrix());
    shader->setUniform(GLShader::ColorUniform::Color, outlineColor());
    shader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

    // The shader writes premultiplied alpha, and the state is left as it was
    // found: everything painted after this expects to set up its own blending.
    const bool blending = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    vbo->render(m_paintRegion, GL_TRIANGLES, true);
    if (!blending) {
        glDisable(GL_BLEND);
    }
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
