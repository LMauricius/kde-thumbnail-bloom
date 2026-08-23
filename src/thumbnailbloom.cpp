/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailbloom.h"
#include "thumbnailbend.h"
#include "thumbnailbloomconfig.h"
#include "thumbnailoverlay.h"

#include <core/colorspace.h>
#include <core/output.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <cursor.h>
#include <effect/effecthandler.h>
#include <options.h>
#include <window.h>
#include <workspace.h>
#include <effect/effectwindow.h>
#include <opengl/glutils.h>

#include <KColorScheme>

#include <QHash>
#include <QSet>
#include <QTransform>
#include <QVector2D>

#include <algorithm>
#include <array>
#include <vector>

using namespace KWin;

namespace ThumbnailBloom {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/*!
 * Whether the layout holds still while a window is being dragged.
 *
 * Thumbnails rearranging under a moving window is a lot of motion for something
 * the user is not looking at, so the whole pass waits for the drag to end. Not
 * configurable yet, hence the constant rather than a setting.
 */
constexpr bool reducedMotion = true;

/*!
 * How finely a bent thumbnail is cut up before its vertices are moved.
 *
 * Texture coordinates are interpolated linearly inside a quad, so a quad drawn
 * as a trapezoid would still be textured as if it were a rectangle. Splitting
 * the window into a grid this size leaves each cell small enough for that error
 * to disappear, which is what makes the pixels perspective correct.
 */
constexpr int bendSubdivisions = 16;

/*! The frame geometry of \a w as a QRectF, the rectangle all the geometry here runs on. */
static QRectF frameRect(const EffectWindow *w) { return QRectF(w->frameGeometry()); }

/*!
 * Returns the side the thumbnail of \a w resting at \a rect turns away towards.
 *
 * The window's real place, seen from the thumbnail: a thumbnail leans towards
 * the window it belongs to, which points at where clicking it leads. Only the
 * direction is taken, the angle itself comes from the settings; there is an idea
 * to make the angle dynamic as well, from that same distance.
 */
static QVector2D bendDirection(EffectWindow *w, const QRectF &rect)
{
    const QPointF offset = frameRect(w).center() - rect.center();
    QVector2D direction(offset.x(), offset.y());
    direction.normalize();
    return direction;
}

template <typename T>
void ThumbnailBloomEffect::Animated<T>::interpolate(qreal progress)
{
    current = from * (1.0 - progress) + to * progress;
}

// QRectF has no arithmetic of its own, so the rectangle channel blends by
// component.
template <>
void ThumbnailBloomEffect::Animated<QRectF>::interpolate(qreal progress)
{
    const qreal inverse = 1.0 - progress;
    current = QRectF(from.x() * inverse + to.x() * progress, from.y() * inverse + to.y() * progress,
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
    QRectF grown(
        QPointF(rect.center().x() - size.width() / 2, rect.center().y() - size.height() / 2), size);

    // Only shift the result: resizing it again would break the superset property.
    grown.moveLeft(std::min(
        std::max(grown.left(), area.left()), std::max(area.right() - grown.width(), area.left())));
    grown.moveTop(std::min(
        std::max(grown.top(), area.top()), std::max(area.bottom() - grown.height(), area.top())));
    return grown;
}

/*!
 * Returns how much larger than its resting rectangle \a current is drawn.
 *
 * One while the thumbnail sits where the layout put it, more while the hover
 * has it grown; it is what orders the lifted thumbnails among themselves.
 */
static qreal growth(const QRectF &current, const QRectF &base)
{
    return base.width() > 0 ? current.width() / base.width() : 1.0;
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
        rect.y() + (expanded.y() - natural.y()) * scaleY, expanded.width() * scaleX,
        expanded.height() * scaleY);
}

//! Width of the hover outline, in logical pixels.
constexpr qreal outlineWidth = 2.0;

//! How far past its resting growth of 1 a thumbnail must be drawn to count as lifted.
constexpr qreal liftEpsilon = 1e-3;

/*!
 * Returns the two triangles covering the quad \a corners, appended to \a vertices.
 *
 * The corners run clockwise from the top left, the order bendQuad() gives them
 * in, and nothing here assumes the shape is a rectangle: a bent outline is four
 * trapezoids.
 */
static void appendQuad(std::vector<QVector2D> &vertices, const BendQuad &corners)
{
    const QVector2D topLeft(corners[0]);
    const QVector2D topRight(corners[1]);
    const QVector2D bottomRight(corners[2]);
    const QVector2D bottomLeft(corners[3]);
    vertices.insert(
        vertices.end(), { topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight });
}

/*! Returns the outline colour of the current colour scheme. */
static QColor outlineColor()
{
    // Read on every use: the colour scheme can change while the effect runs.
    return KColorScheme(QPalette::Active, KColorScheme::View)
        .decoration(KColorScheme::FocusColor)
        .color();
}

/*!
 * Returns whether \a w holds a pointer grab: a menu or any other surface the
 * client put up with an explicit grab, which owns the input while it is mapped
 * and is dismissed by the first click outside it.
 */
static bool grabsInput(EffectWindow *w)
{
    // hasPopupGrab() lives on KWin::Window, not on the effect window.
    const Window *window = w->window();
    return window && window->hasPopupGrab();
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
    return grabsInput(w) || w->internalWindow() || w->isDock() || w->isPopupWindow()
        || w->isPopupMenu() || w->isDropdownMenu() || w->isMenu() || w->isAppletPopup()
        || w->isNotification() || w->isCriticalNotification() || w->isOnScreenDisplay()
        || w->isTooltip() || w->isComboBox() || w->isDNDIcon() || w->isSplash()
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

/*! Whether the user is dragging a window around right now. */
static bool userMoveInProgress()
{
    const Window *window = workspace()->moveResizeWindow();
    return window && window->isInteractiveMove();
}

/*! Whether the layout holds still right now: reduced motion during a drag. */
static bool layoutFrozen() { return reducedMotion && userMoveInProgress(); }

/*!
 * Keeps the internal window behind \a handle out of every list of windows the
 * user can see.
 *
 * KWin hands an internal window to the rest of the session as an ordinary one,
 * so the effect's shields and click targets turn up in anything that walks the
 * window list and does not ask whether a window is a real client: the Overview
 * and Window View heaps, task managers, pagers. (The task switcher does ask, so
 * it never showed them.) The three skip flags are what those lists honour, and
 * the close animation flag keeps the other effects from playing anything when a
 * target is dropped.
 */
static void hideFromWindowLists(QWindow *handle)
{
    Window *window = workspace()->findInternal(handle);
    if (!window) {
        return;
    }

    window->setSkipTaskbar(true);
    window->setSkipPager(true);
    window->setSkipSwitcher(true);
    window->setSkipCloseAnimation(true);
}

/*!
 * Shows \a window and takes it back out of the window lists. The two go
 * together: hiding an internal window destroys the KWin::Window behind it, so
 * every show() makes a fresh one with the default flags back. One that never
 * went away still carries the flags it was given, and the relayout comes round
 * often enough for the walk of the window list to be worth skipping.
 */
static void showOverlay(QRasterWindow *window)
{
    if (window->isVisible()) {
        return;
    }

    window->show();
    hideFromWindowLists(window);
}

/*! Sets the mask of \a window to \a mask, unless that is what it already is. */
static void setOverlayMask(QRasterWindow *window, const QRegion &mask)
{
    // Qt hands every mask straight to the platform window without looking, and
    // KWin turns one into an input region of its own; most relayouts leave it
    // exactly as it was.
    if (window->mask() != mask) {
        window->setMask(mask);
    }
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
{
    input()->installInputEventFilter(&m_shieldFilter);
    input()->installInputEventFilter(&m_touchDragFilter);

    // Changes tend to arrive in bursts (a raise is a stacking change plus an
    // activation plus a geometry change), so they only mark the layout dirty.
    m_relayoutTimer.setSingleShot(true);
    m_relayoutTimer.setInterval(0);
    connect(&m_relayoutTimer, &QTimer::timeout, this, &ThumbnailBloomEffect::relayout);

    connect(effects, &EffectsHandler::windowAdded, this, [this](EffectWindow *w) {
        // The first internal window to appear while a window menu is being
        // opened is that menu; see openWindowMenu().
        if (m_menuOwner && !m_menuPopup && w->internalWindow()) {
            m_menuPopup = w;
        }
        watch(w);
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::windowClosed, this, [this](EffectWindow *w) {
        if (w == m_menuPopup) {
            m_menuPopup = nullptr;
            m_menuOwner = nullptr;
        }
        forget(w);
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::windowDeleted, this, [this](EffectWindow *w) { forget(w); });
    connect(effects, &EffectsHandler::windowActivated, this,
        [this](EffectWindow *) { scheduleRelayout(); });
    connect(effects, &EffectsHandler::stackingOrderChanged, this,
        &ThumbnailBloomEffect::scheduleRelayout);
    // No per-window signal exists for the "show desktop" hidden flag.
    connect(effects, &EffectsHandler::showingDesktopChanged, this,
        [this](bool) { scheduleRelayout(); });
    connect(effects, &EffectsHandler::currentActivityChanged, this,
        [this](const QString &) { scheduleRelayout(); });
    connect(effects, &EffectsHandler::desktopChanged, this,
        [this](VirtualDesktop *, VirtualDesktop *, EffectWindow *, LogicalOutput *) {
            scheduleRelayout();
        });
    connect(effects, &EffectsHandler::screenAdded, this,
        [this](LogicalOutput *) { scheduleRelayout(); });
    connect(effects, &EffectsHandler::screenRemoved, this,
        [this](LogicalOutput *) { scheduleRelayout(); });

    // Hover is tracked from the cursor position rather than from the overlays:
    // KWin dispatches pointer events to internal windows on its own and never
    // synthesises the enter and leave events a QWindow would otherwise receive.
    connect(Cursors::self(), &Cursors::positionChanged, this,
        [this](Cursor *, const QPointF &pos) { updateHover(pos); });

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
    m_skipOnAllDesktops = ThumbnailBloomConfig::skipOnAllDesktops();
    m_skipMaximized = ThumbnailBloomConfig::skipMaximized();
    m_skipParents = ThumbnailBloomConfig::skipParents();
    m_skipChildren = ThumbnailBloomConfig::skipChildren();

    m_layoutOptions.initialScale
        = std::clamp(ThumbnailBloomConfig::initialSize() / 100.0, 0.1, 1.0);
    m_layoutOptions.minScale = std::clamp(
        ThumbnailBloomConfig::minimumSize() / 100.0, 0.05, m_layoutOptions.initialScale);
    m_layoutOptions.scaleStep = 0.05;
    m_layoutOptions.margin = 8;
    m_layoutOptions.minOccludedFraction
        = std::clamp(ThumbnailBloomConfig::minimumOcclusion() / 100.0, 0.01, 1.0);

    m_showIcons = ThumbnailBloomConfig::showIcons();
    m_showTitles = ThumbnailBloomConfig::showTitles();

    m_thumbnailOpacity = std::clamp(ThumbnailBloomConfig::opacity() / 100.0, 0.1, 1.0);

    // Redirecting a window into a texture costs a render pass per frame, so it
    // is only done while there is a bend to draw; turning the angle down to zero
    // hands every window that is already blooming back to the ordinary path.
    m_bendAngle = std::clamp<qreal>(ThumbnailBloomConfig::bendAngle(), 0.0, 60.0);
    for (auto &[w, state] : m_states) {
        setRedirected(w, state, m_bendAngle > 0.0);
    }

    // The system's animation speed is already folded into animationTime().
    m_animationDuration
        = std::max(std::chrono::milliseconds(1), animationTime(std::chrono::milliseconds(250)));

    scheduleRelayout();
}

void ThumbnailBloomEffect::watch(EffectWindow *w)
{
    // Anything that can change what covers what invalidates the layout.
    connect(w, &EffectWindow::windowFrameGeometryChanged, this,
        &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowMaximizedStateChanged, this,
        &ThumbnailBloomEffect::scheduleRelayout);
    connect(
        w, &EffectWindow::windowFullScreenChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(
        w, &EffectWindow::windowKeepAboveChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::minimizedChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowDesktopsChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowHiddenChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowModalityChanged, this, &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowStartUserMovedResized, this,
        &ThumbnailBloomEffect::scheduleRelayout);
    connect(w, &EffectWindow::windowFinishUserMovedResized, this,
        &ThumbnailBloomEffect::scheduleRelayout);

    // A thumbnail is painted away from the window it belongs to, so the damage
    // KWin schedules for the window itself does not cover it.
    connect(w, &EffectWindow::windowDamaged, this, [this](EffectWindow *window) {
        const auto it = m_states.find(window);
        if (it != m_states.end()) {
            effects->addRepaint(
                RectF(thumbnailBounds(window, it->second.rect.current).adjusted(-1, -1, 1, 1)));
        }
    });
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ThumbnailBloomEffect::scheduleRelayout() { m_relayoutTimer.start(); }

void ThumbnailBloomEffect::relayout()
{
    // Placing the click targets needs to know what covers them, so this comes
    // first.
    updateSystemRegion();

    // Reduced motion: a drag freezes the layout. Every thumbnail keeps the
    // rectangle it already has and nothing new blooms, so the only window that
    // is retargeted is the dragged one itself, which is on its way back to its
    // real geometry and has to keep following the pointer. The finish signal
    // schedules the pass that catches the layout up.
    if (layoutFrozen()) {
        for (auto &[w, state] : m_states) {
            retarget(w, w->isUserMove() ? frameRect(w) : QRectF(state.base));
        }
        updateShields();
        updateHover(effects->cursorPos());
        return;
    }

    // Which windows take part at all is asked once and kept, in stacking order:
    // the transient parents have to be complete before the first window can be
    // judged, and walking the whole stack twice over to get that would ask it of
    // every window twice.
    std::vector<EffectWindow *> relevant;
    for (EffectWindow *w : effects->stackingOrder()) {
        if (isRelevant(w)) {
            relevant.push_back(w);
        }
    }

    const QSet<EffectWindow *> parents = transientParents(relevant);

    // Windows only ever collide with windows of their own screen, so each screen
    // is laid out on its own. The stacking order is preserved per screen.
    EffectWindow *active = effects->activeWindow();
    QHash<LogicalOutput *, QList<LayoutWindow>> perScreen;
    for (EffectWindow *w : relevant) {
        // Worked out once and handed to both: being ignored is the larger half
        // of being ineligible, and it is the half that can cost a screen lookup.
        const bool ignored = isIgnored(w, parents);
        perScreen[w->screen()].append(
            LayoutWindow { w, frameRect(w), isEligible(w, ignored), w == active, ignored });
    }

    applyPlacements(perScreen);
}

QSet<EffectWindow *> ThumbnailBloomEffect::transientParents(
    const std::vector<EffectWindow *> &relevant) const
{
    // Only a real window makes its owner a parent. A menu is transient for
    // the window it was opened in, so counting it would turn that window
    // into a skipped parent for as long as the menu is up.
    QSet<EffectWindow *> parents;
    for (EffectWindow *w : relevant) {
        const QList<EffectWindow *> mainWindows = w->mainWindows();
        for (EffectWindow *parent : mainWindows) {
            parents.insert(parent);
        }
    }
    return parents;
}

void ThumbnailBloomEffect::applyPlacements(
    const QHash<LogicalOutput *, QList<LayoutWindow>> &perScreen)
{
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
            retarget(w, frameRect(w));
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

    // A window travelling back to its real geometry is on its way to being an
    // ordinary window again, so it fades back to fully opaque just like the
    // hovered thumbnail does.
    const bool thumbnail = !sameRect(base, frameRect(w));
    const QRectF target = state.hovered
        ? grownRect(base, frameRect(w), QRectF(effects->clientArea(MaximizeArea, w)))
        : base;
    const qreal targetOpacity = (thumbnail && !state.hovered) ? m_thumbnailOpacity : 1.0;
    // The caption belongs to the resting thumbnail only: it fades out under the
    // pointer, and on the way back to the real window it is gone before the
    // window is itself again.
    const qreal targetCaption = (thumbnail && !state.hovered) ? 1.0 : 0.0;
    // The bend belongs to the resting thumbnail just as the caption does: it
    // flattens out under the pointer, so a hovered window is seen head on, and it
    // is gone before the window is back where it really is.
    const qreal targetBend = targetCaption;

    // Nothing is bent unless it is painted through an offscreen texture, and
    // that is decided per window rather than once, since a state can be inserted
    // long after the effect was configured. It stays up for as long as the
    // window blooms, flat moments included: dropping it whenever the bend
    // reaches zero would hand the window back and forth between the offscreen
    // path and the ordinary one on every hover, and the two do not compose a
    // scaled down window quite alike.
    setRedirected(w, state, m_bendAngle > 0.0);

    // The click target follows the resting rectangle, not the animation: a
    // thumbnail can be hovered and clicked from the moment it sets off, but only
    // where it is going to end up.
    updateOverlay(w, state);

    if (inserted) {
        state.rect.snap(frameRect(w));
        state.opacity.snap(1.0);
        state.caption.snap(0.0);
        state.bend.snap(0.0);
    } else if (sameRect(state.rect.to, target) && qFuzzyCompare(state.opacity.to, targetOpacity)
        && qFuzzyCompare(state.caption.to, targetCaption)
        && qFuzzyCompare(state.bend.to, targetBend)) {
        return;
    }

    // Every retarget starts a whole new trip, from where the thumbnail is right
    // now and lasting a full animation duration. Keeping the old start and the
    // elapsed time instead would make a change of destination land the thumbnail
    // somewhere it never was (a hover reversed halfway jumps straight to the
    // resting rectangle) and leave it whatever is left of the duration to get
    // there, so a reversal late in the animation would be over in a couple of
    // frames.
    //
    // What must not come back with the restart is the slow start of the easing
    // curve: a target that keeps moving (a window being dragged over the
    // thumbnail) retargets on every frame, and easing in from a standstill each
    // time leaves the thumbnail crawling behind it. A thumbnail that is already
    // in motion therefore switches to a curve that starts at full speed and only
    // eases out, which picks up where the previous trip left off closely enough
    // for the eye and follows a moving target frame by frame.
    state.rect.restart(target);
    state.opacity.restart(targetOpacity);
    state.caption.restart(targetCaption);
    state.bend.restart(targetBend);
    state.timeline.setEasingCurve(
        (inserted || state.timeline.done()) ? QEasingCurve::InOutCubic : QEasingCurve::OutCubic);
    state.timeline.setDuration(m_animationDuration);
    state.timeline.reset();

    m_animating = true;
    effects->addRepaintFull();
}

EffectWindow *ThumbnailBloomEffect::menuOwner() const
{
    return (m_menuOwner && m_states.count(m_menuOwner)) ? m_menuOwner : nullptr;
}

void ThumbnailBloomEffect::openWindowMenu(EffectWindow *w, const QPointF &pos)
{
    Window *window = w->window();
    if (!window) {
        return;
    }

    // KWin offers no way to ask whether the window menu is open (UserActionsMenu
    // is not part of the installed headers), so the menu is recognised by its
    // own window: it is an internal window and it is created inside the command
    // below, which shows it synchronously. From there windowClosed says when it
    // is gone. If nothing was added, no menu appeared and there is nothing to
    // keep focused either.
    m_menuOwner = w;
    m_menuPopup = nullptr;
    window->performMousePressCommand(Options::MouseOperationsMenu, pos);
    if (!m_menuPopup) {
        m_menuOwner = nullptr;
    }
    scheduleRelayout();
}

void ThumbnailBloomEffect::updateHover(const QPointF &pos)
{
    // Everything with a placed click target takes part, animating or not: the
    // click target sits on the destination of the thumbnail, so the hit test
    // never follows it along its path.
    const auto targetable
        = [](const BloomState &state) { return state.overlay && state.overlay->isVisible(); };

    // Reduced motion: a thumbnail growing under the pointer while a window is
    // being dragged is motion nobody asked for, since the pointer is only
    // passing over it on its way somewhere else. Nothing is hovered until the
    // drag ends, and the pass the finish signal schedules picks the hover back
    // up from wherever the pointer came to rest.
    if (layoutFrozen()) {
        for (const auto &[w, state] : m_states) {
            setHovered(w, false);
        }
        return;
    }

    // The menu of a thumbnail belongs to that thumbnail. It is a popup, so it
    // takes the pointer and is cut out of the hit region, and the thumbnail
    // would shrink away under its own menu; it stays focused until the menu is
    // gone instead. The menu closing is a window closing, which schedules the
    // relayout that ends this.
    if (EffectWindow *owner = menuOwner()) {
        for (const auto &[w, state] : m_states) {
            setHovered(w, w == owner);
        }
        return;
    }

    EffectWindow *hovered = thumbnailUnder(pos);
    for (const auto &[w, state] : m_states) {
        if (targetable(state)) {
            setHovered(w, w == hovered);
        }
    }
}

EffectWindow *ThumbnailBloomEffect::thumbnailUnder(const QPointF &pos) const
{
    // The hit test is against the exposed part of the resting rectangle, never
    // the grown one: the pointer keeps the thumbnail enlarged only while it
    // stays inside the area the thumbnail occupies when it is not hovered.
    // Testing the grown rectangle instead would make the thumbnail hold on to
    // the pointer over an area it only covers because of that very pointer.
    // What a panel or a popup covers belongs to that panel or popup, so it is
    // cut out of the region and hovering there does nothing.
    // At most one thumbnail is hovered, and the hovered one keeps it as long as
    // the pointer stays on it.
    // A window painted over the thumbnail takes that part of it away, the hover
    // included: the pointer is on the window, not on a thumbnail it cannot see.
    EffectWindow *hovered = nullptr;
    for (const auto &[w, state] : m_states) {
        if (state.overlay && state.overlay->isVisible() && state.hitRegion.contains(pos.toPoint())
            && !m_shieldFilter.isCovered(w->window(), pos) && (!hovered || state.hovered)) {
            hovered = w;
        }
    }
    return hovered;
}

void ThumbnailBloomEffect::setHovered(EffectWindow *w, bool hovered)
{
    const auto it = m_states.find(w);
    if (it == m_states.end() || it->second.hovered == hovered) {
        return;
    }

    it->second.hovered = hovered;

    // Nothing is lifted from here: the lift follows the size the thumbnail is
    // actually drawn at, which the paint pass works out for itself. That is what
    // keeps a thumbnail up while it shrinks back, the hover being long gone by
    // then.

    // Never retarget from here. This runs either inside KWin's pointer dispatch
    // (through the cursor position signal) or inside the paint pass, and
    // retargeting hides the overlay: hiding an internal window makes KWin destroy
    // it right away, under the very code that is still using it. The relayout
    // timer moves that to a safe point of the event loop instead.
    scheduleRelayout();
}

void ThumbnailBloomEffect::forget(EffectWindow *w)
{
    std::erase(m_lifted, w);
    if (m_menuOwner == w) {
        m_menuOwner = nullptr;
        m_menuPopup = nullptr;
    }

    // Extracted rather than erased in place: destroying the overlay makes KWin
    // emit windowClosed for its internal window synchronously, and that handler
    // calls back into m_states, which has to be consistent by then. The overlay
    // itself only dies on the next event loop pass, because this can run under
    // input dispatch, where destroying an internal window is not survivable;
    // its signals are cut right away so a click in the meantime cannot reach
    // the window pointer that is about to go stale.
    // The offscreen texture goes with the state. The window may already be gone
    // here (this also runs on windowClosed and windowDeleted), which is why the
    // flag is asked rather than the effect being told to unredirect blindly.
    const auto it = m_states.find(w);
    if (it != m_states.end()) {
        setRedirected(w, it->second, false);

        // The handles go with the state, so that nothing is left claiming a
        // surface that is on its way out.
        m_captionTargets.erase(it->second.overlay.get());
        m_ownOverlays.erase(it->second.overlay.get());
        m_ownOverlays.erase(it->second.shield.get());
    }

    auto node = m_states.extract(w);
    if (!node.empty()) {
        for (OverlayWindow *window :
            { static_cast<OverlayWindow *>(node.mapped().overlay.release()),
                node.mapped().shield.release() }) {
            if (window) {
                window->disconnect();
                window->deleteLater();
            }
        }
    }
    effects->addRepaintFull();
}

void ThumbnailBloomEffect::startThumbnailMove(EffectWindow *w, const QPointF &pos, qint32 touchId)
{
    const auto it = m_states.find(w);
    Window *window = w->window();
    if (it == m_states.end() || !window) {
        return;
    }

    // The window is dragged out of its thumbnail, so that is where it starts:
    // its own size, centred on the rectangle the pointer or the finger is
    // actually on, kept inside the work area.
    QRectF target(QPointF(), frameRect(w).size());
    target.moveCenter(it->second.rect.current.center());
    target = window->keepInArea(target, effects->clientArea(MaximizeArea, w));

    // Activating first is what makes the drag count as using the window; the
    // relayout it schedules then animates the thumbnail into the geometry set
    // here instead of sending it back to where the window used to be. A window
    // that cannot be moved at all still gets that much out of the gesture.
    effects->activateWindow(w);
    if (!window->isMovable()) {
        return;
    }
    window->move(target.topLeft());

    // MouseMove takes its grab offset as a fraction of the geometry the window
    // has at this very moment, so moving it beforehand is what keeps it from
    // jumping once the pointer starts driving it. From here KWin's own move
    // filter follows the pointer; a touch sequence has to be fed by the effect,
    // because that filter only follows a point it saw go down.
    window->performMousePressCommand(Options::MouseMove, pos);
    if (touchId >= 0) {
        m_touchDragFilter.arm(window, touchId);
    }
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

    const bool goingHome = sameRect(state.base, frameRect(w));
    if (goingHome || state.hitRegion.isEmpty()) {
        state.hitRegion = QRegion();
        if (state.overlay) {
            // A thumbnail on its way back to its window keeps its click target
            // up, where it is, for as long as the caption is still fading out on
            // it. It must not act as a thumbnail any more though, so it is made
            // output only; the state (and with it the window) is dropped once
            // the trip ends.
            if (goingHome && state.caption.current > 0) {
                state.overlay->setOutputOnly(true);
            } else {
                state.overlay->hide();
                state.overlayWindow = nullptr;
            }
        }
        return;
    }

    if (!state.overlay) {
        state.overlay = std::make_unique<ThumbnailOverlay>();
        // Registered before it is ever shown: from the moment it is, the paint
        // pass and the layout both have to recognise it for one of its own.
        m_ownOverlays.insert(state.overlay.get());
        m_captionTargets.insert(state.overlay.get());
        connect(state.overlay.get(), &ThumbnailOverlay::activated, this,
            [w]() { effects->activateWindow(w); });
        connect(state.overlay.get(), &ThumbnailOverlay::dragStarted, this,
            [this, w](const QPointF &pos, qint32 touchId) { startThumbnailMove(w, pos, touchId); });
        // The menu command does not activate the window, which is the point: a
        // right click is a question about the thumbnail, not a use of it.
        connect(state.overlay.get(), &ThumbnailOverlay::menuRequested, this,
            [this, w](const QPointF &pos) { openWindowMenu(w, pos); });
    }

    state.overlay->setOutputOnly(false);

    // The caption is drawn by the click target, which is the one surface of a
    // thumbnail the compositor paints exactly once per frame.
    state.overlay->setCaption(
        m_showIcons ? w->icon() : QIcon(), m_showTitles ? w->caption() : QString());
    state.overlay->setCaptionOpacity(state.caption.current);

    // The resting rectangle, never the current one: the click target must not
    // travel with the animation, and never grows with the hover either.
    state.overlay->setGeometry(rect);
    setOverlayMask(state.overlay.get(), state.hitRegion.translated(-rect.topLeft()));
    showOverlay(state.overlay.get());

    // Every show() makes a fresh window of the click target, so the one the
    // scene knows is picked up here rather than looked up again on every frame
    // that paints a caption.
    state.overlayWindow = effects->findWindow(state.overlay.get());
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
    // rather than on the window, which never sees an enter event at all. The
    // shield does not answer the event itself, ShieldFilter hands it on to
    // whatever is really below; only clicks on the thumbnail activate a bloomed
    // window.

    // Whatever the thumbnails claim stays theirs; the shields are internal
    // windows too, and two of those on the same pixel have no defined order.
    // The filter is told which window each one belongs to as well, so that it
    // can ask whether the thumbnail is visible at all where an event lands.
    QRegion thumbnails;
    QList<ShieldFilter::Thumbnail> thumbnailAreas;
    for (const auto &[w, state] : m_states) {
        if (state.hitRegion.isEmpty()) {
            continue;
        }
        thumbnails += state.hitRegion;
        thumbnailAreas.append(ShieldFilter::Thumbnail { w->window(), state.hitRegion });
    }

    // Walking the stack top down keeps a shield inside the area where its window
    // really is the topmost input target: everything above it has been added to
    // `covered` by the time the window is reached. Covering a window that lies
    // over a bloomed one would take away input that rightfully belongs to it.
    QRegion covered = m_systemRegion;
    QRegion shieldRegion;
    QSet<EffectWindow *> shielded;
    QSet<Window *> bloomedWindows;
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

            // Every bloomed window has to be skipped when the input is handed
            // on, shielded or not: one that is covered everywhere still has to
            // stay out of the way under somebody else's shield.
            bloomedWindows.insert(w->window());

            const QRegion exposed = placeShield(state, frame, covered + thumbnails);
            if (!exposed.isEmpty()) {
                shielded.insert(w);
                shieldRegion += exposed;
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

    m_shieldFilter.setState(shieldRegion, bloomedWindows, thumbnailAreas);
}

QRegion ThumbnailBloomEffect::placeShield(
    BloomState &state, const QRect &frame, const QRegion &covered)
{
    const QRegion exposed = QRegion(frame) - covered;
    if (exposed.isEmpty()) {
        return exposed;
    }
    if (!state.shield) {
        state.shield = std::make_unique<OverlayWindow>();
        // Registered before it is ever shown: from the moment it is, the rest of
        // the effect has to recognise it for one of its own.
        m_ownOverlays.insert(state.shield.get());
    }
    const QRect bounds = exposed.boundingRect();
    state.shield->setGeometry(bounds);
    setOverlayMask(state.shield.get(), exposed.translated(-bounds.topLeft()));
    showOverlay(state.shield.get());
    return exposed;
}

// ---------------------------------------------------------------------------
// Window classification
// ---------------------------------------------------------------------------

bool ThumbnailBloomEffect::isRelevant(EffectWindow *w) const
{
    // "Show desktop" hides windows without minimising them, and a window that is
    // not on screen must not get a thumbnail either.
    if (w->isDeleted() || w->isMinimized() || w->isHidden() || w->isHiddenByShowDesktop()
        || !w->screen()) {
        return false;
    }
    if (!w->isOnCurrentDesktop() || !w->isOnCurrentActivity()) {
        return false;
    }
    // Whatever is painted above the ordinary windows, menus and anything else
    // holding a grab included, is out of the effect altogether: it neither
    // blooms nor pushes anything into bloom.
    if (isSystemElement(w) || w->isDesktop()) {
        return false;
    }
    if (w->isUtility() || w->isToolbar() || w->isOutline() || w->isInputMethod()) {
        return false;
    }

    return w->isNormalWindow() || w->isDialog();
}

// Both of these are asked once per internal window in the paint pass and again
// for every window of the stack in the layout, so they go through a set of the
// handles rather than through the states.

bool ThumbnailBloomEffect::isCaptionTarget(EffectWindow *w) const
{
    const QWindow *handle = w->internalWindow();
    // The shields paint nothing, so only the click targets are of interest.
    return handle && m_captionTargets.contains(handle);
}

bool ThumbnailBloomEffect::isOwnOverlay(EffectWindow *w) const
{
    const QWindow *handle = w->internalWindow();
    return handle && m_ownOverlays.contains(handle);
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

bool ThumbnailBloomEffect::isIgnored(EffectWindow *w, const QSet<EffectWindow *> &parents) const
{
    if (m_skipKeepAbove && w->keepAbove()) {
        return true;
    }
    if (m_skipOnAllDesktops && w->isOnAllDesktops()) {
        return true;
    }
    if (m_skipMaximized && isMaximized(w)) {
        return true;
    }
    if (m_skipChildren && w->transientFor()) {
        return true;
    }
    if (m_skipParents && parents.contains(w)) {
        return true;
    }

    return false;
}

bool ThumbnailBloomEffect::isEligible(EffectWindow *w, bool ignored) const
{
    // The window being worked in, or the one under the pointer's grab, is no
    // candidate either, but unlike an ignored one it still hides what it covers.
    if (w == effects->activeWindow() || w->isUserMove() || w->isUserResize()) {
        return false;
    }

    return !ignored;
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

void ThumbnailBloomEffect::setRedirected(EffectWindow *w, BloomState &state, bool redirected)
{
    redirected = redirected && OffscreenEffect::supported();
    if (state.redirected == redirected) {
        return;
    }

    state.redirected = redirected;
    if (redirected) {
        redirect(w);
    } else {
        unredirect(w);
    }
}

QTransform ThumbnailBloomEffect::stateBend(
    EffectWindow *w, const BloomState &state, const QRectF &rect) const
{
    return bendTransform(
        rect, m_bendAngle * state.bend.current, bendDirection(w, state.rect.current));
}

QRectF ThumbnailBloomEffect::paintedArea(EffectWindow *w, const BloomState &state) const
{
    constexpr qreal pad = outlineWidth + 1.0;

    const QRectF rect = state.rect.current;
    QRectF bounds = thumbnailBounds(w, rect);

    if (state.bend.current > 0.0 && !rect.isEmpty()) {
        // A projective map takes straight lines to straight lines, so the four
        // mapped corners bound the whole of the mapped rectangle. The unbent
        // bounds stay in the union as a floor: the bend fits the frame back into
        // its own rectangle, so the thumbnail proper never leaves it, and only
        // what lies outside the frame can be thrown either way.
        const QRectF bent = stateBend(w, state, rect).mapRect(bounds);
        if (bent.isValid()) {
            bounds = bounds.united(bent);
        }
    }

    return bounds.adjusted(-pad, -pad, pad, pad);
}

void ThumbnailBloomEffect::apply(
    EffectWindow *window, int /*mask*/, WindowPaintData & /*data*/, WindowQuadList &quads)
{
    const auto it = m_states.find(window);
    if (it == m_states.end() || it->second.bend.current <= 0.0) {
        return;
    }

    const BloomState &bloomState = it->second;

    // Window coordinates: the frame geometry sits at the origin and everything
    // painted around it (the shadow, the decoration) reaches outside it, into
    // negative coordinates above and to the left. The bend is worked out on the
    // frame alone and the scale and the translation that put the thumbnail on the
    // screen are applied to the result afterwards, by applyTransform().
    const QRectF frame(QPointF(0, 0), frameRect(window).size());
    if (frame.isEmpty()) {
        return;
    }

    const QTransform transform = stateBend(window, bloomState, frame);

    // The transform is projective, so mapping a vertex through it is the whole
    // perspective: what the subdivision adds is that every cell of the grid gets
    // its own corners mapped, and the texture inside it is stretched between
    // them instead of across the window as a whole. Everything outside the frame
    // rides along on the same map, which keeps the shadow attached to the edge
    // it belongs to.
    quads = quads.makeRegularGrid(bendSubdivisions, bendSubdivisions);
    for (WindowQuad &quad : quads) {
        for (int i = 0; i < 4; ++i) {
            WindowVertex &vertex = quad[i];
            const QPointF mapped = transform.map(QPointF(vertex.x(), vertex.y()));
            vertex.setX(mapped.x());
            vertex.setY(mapped.y());
        }
    }
}

void ThumbnailBloomEffect::applyTransform(
    EffectWindow *w, const BloomState &state, WindowPaintData &data) const
{
    const QRectF natural = w->frameGeometry();
    if (natural.width() <= 0 || natural.height() <= 0) {
        return;
    }

    // Scaling happens around the window's top left corner, so the translation is
    // expressed in unscaled screen coordinates.
    data.setScale(QVector2D(state.rect.current.width() / natural.width(),
        state.rect.current.height() / natural.height()));
    data.setXTranslation(state.rect.current.x() - natural.x());
    data.setYTranslation(state.rect.current.y() - natural.y());
    data.multiplyOpacity(state.opacity.current);
}

void ThumbnailBloomEffect::prePaintScreen(ScreenPrePaintData &data)
{
    const std::vector<EffectWindow *> settledBack = advanceAnimations(data);
    updateLift();

    // An empty damage is left alone throughout: no pass is drawing anything, and
    // widening one would make a frame out of a pass that was going to paint
    // nothing at all.
    if (!data.paint.isEmpty()) {
        // A lifted thumbnail is drawn out of turn, right after its anchor, so
        // the two have to be painted in the same pass. Partial damage does not
        // guarantee that: a repaint of the area the pointer moved through paints
        // the windows below the thumbnail without ever reaching the anchor, and
        // the thumbnail is erased wherever that happens. The whole screen is
        // taken instead.
        //
        // Widening the damage of a pass that is happening anyway, rather than
        // asking for a repaint once the pass is over, is what keeps a lifted
        // thumbnail that has come to rest from holding the compositor at a full
        // repaint per frame for as long as the pointer stays on it: nothing
        // changes on the screen then, so nothing has to be drawn until something
        // else damages it.
        if (!m_lifted.empty() && data.screen) {
            data.paint |= Region(data.screen->geometry());
        } else if (!m_dirty.isEmpty()) {
            // The animations only need the ground they are moving over. What is
            // asked for here is this frame; postPaintScreen() asks for the next
            // one, which widens the damage again from wherever they got to.
            data.paint |= Region(m_dirty);
        }
    }

    // forget(), not erase(): the click target may still be up, painting the last
    // of the caption, and an internal window may not be destroyed from inside the
    // effect chain. forget() cuts its signals now and lets it die on the next
    // event loop pass.
    for (EffectWindow *w : settledBack) {
        forget(w);
    }

    Effect::prePaintScreen(data);
}

std::vector<EffectWindow *> ThumbnailBloomEffect::advanceAnimations(ScreenPrePaintData &data)
{
    m_animating = false;
    m_dirty = QRegion();

    std::vector<EffectWindow *> settledBack;
    for (auto &[w, state] : m_states) {
        const QRectF before = state.painted;

        state.timeline.advance(data.view);
        const qreal progress = state.timeline.value();
        state.rect.interpolate(progress);
        state.opacity.interpolate(progress);
        state.caption.interpolate(progress);
        state.bend.interpolate(progress);
        if (state.overlay) {
            state.overlay->setCaptionOpacity(state.caption.current);
        }

        state.painted = paintedArea(w, state);

        if (!state.timeline.done()) {
            m_animating = true;

            // Where the thumbnail was and where it now is, so that the frame
            // erases the one and draws the other. The two are taken together
            // rather than as a pair, which covers the ground between them as
            // well: a thumbnail moves in a straight line, and a step large
            // enough to leave a gap would otherwise leave a trail in it. The
            // resting rectangle comes along because the caption is painted
            // there, and the hover can have the thumbnail itself elsewhere.
            m_dirty += before.united(state.painted).toAlignedRect();
            m_dirty += state.base.toAlignedRect();
            continue;
        }

        // A thumbnail that has arrived back at its window is not a thumbnail
        // any more. Click targets are placed by the relayout, not from here.
        if (sameRect(state.rect.to, frameRect(w))) {
            settledBack.push_back(w);
        }
    }

    return settledBack;
}

void ThumbnailBloomEffect::updateLift()
{
    // A thumbnail is lifted while the pointer has it grown, and stays lifted
    // until it has shrunk all the way back: the hover is over the moment the
    // pointer leaves, but the way back takes a whole animation, and dropping the
    // thumbnail behind its neighbours at the first frame of it is a visible jump.
    // The size it is drawn at is the test, so no timeline has to be consulted:
    // between the hover ending and the relayout that retargets the animation the
    // timeline is still the finished one of the way up.
    //
    // They are ordered by that same size, most enlarged last, so the thumbnails
    // still on their way up cover the ones already on their way down.
    m_lifted.clear();
    for (const auto &[w, state] : m_states) {
        if (state.hovered || growth(state.rect.current, state.base) > 1.0 + liftEpsilon) {
            m_lifted.push_back(w);
        }
    }
    std::ranges::sort(m_lifted, [this](EffectWindow *a, EffectWindow *b) {
        const BloomState &sa = m_states.at(a);
        const BloomState &sb = m_states.at(b);
        return growth(sa.rect.current, sa.base) < growth(sb.rect.current, sb.base);
    });

    // The lifted thumbnails are drawn right after the topmost window that covers
    // any of them, and not after the whole screen: everything painted later (the
    // popup layer the outlines live in, and the cursor) keeps painting over them.
    //
    // The click targets count as covering windows here, even though they are
    // above the whole stack rather than at a place in it: they are the surfaces
    // the captions are painted on, and the thumbnail that grew over its
    // neighbours has to cover their captions as well. Its own caption is faded
    // out under the pointer, so nothing of it is lost by drawing over it.
    //
    // Anchoring to a window the thumbnail overlaps also keeps the two in the
    // same repaint: a window that does not intersect the thumbnail may be left
    // out of a partial repaint, and the anchor has to be painted for the
    // thumbnail to be drawn at all.
    //
    // When nothing covers any of them they are already the topmost windows, but
    // they still have to be drawn in one place to be ordered among themselves,
    // so the topmost lifted one becomes the anchor and the set takes its turn.
    m_liftAnchor = nullptr;
    if (!m_lifted.empty()) {
        std::vector<QRect> passed; // thumbnails of the lifted windows already under the walk
        for (EffectWindow *w : effects->stackingOrder()) {
            if (isLifted(w)) {
                // The stacking order runs bottom to top, so only what follows
                // covers the thumbnail; anything below it is already covered.
                passed.push_back(m_states.at(w).rect.current.toAlignedRect());
                m_liftAnchor = w;
            } else if (isRelevant(w)) {
                const QRect frame = w->frameGeometry().toRect();
                if (std::ranges::any_of(
                        passed, [&](const QRect &r) { return frame.intersects(r); })) {
                    m_liftAnchor = w;
                }
            }
        }
    }
    m_liftPending = m_liftAnchor != nullptr;
}

void ThumbnailBloomEffect::prePaintWindow(
    RenderView *view, EffectWindow *w, WindowPrePaintData &data)
{
    const auto it = m_states.find(w);
    if (it != m_states.end()
        && (!sameRect(it->second.rect.current, frameRect(w)) || it->second.opacity.current < 1.0)) {
        // The window is painted somewhere else and at another size, so it may
        // neither be clipped against nor culled by its real geometry.
        data.setTransformed();
        data.setTranslucent();
    }

    Effect::prePaintWindow(view, w, data);
}

void ThumbnailBloomEffect::paintWindow(const RenderTarget &renderTarget,
    const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion,
    WindowPaintData &data)
{
    // A click target is an internal window, and KWin puts every one of those in
    // the topmost layer, so painting it where it is in the stack would show its
    // caption over whatever covers the thumbnail. It is left out here and drawn
    // again right after the window it belongs to, which is what gives the
    // caption the depth of its own thumbnail: from there the compositor covers
    // the two together, and nothing has to be worked out from geometry.
    if (isCaptionTarget(w)) {
        return;
    }

    // The lifted thumbnails are left out at their own places in the stacking
    // order and drawn again after the anchor: not chaining the call is what keeps
    // a window out of a pass. The anchor may be a lifted thumbnail itself, and
    // then its own turn is where the whole set is drawn.
    if (m_liftPending && isLifted(w)) {
        if (w == m_liftAnchor) {
            drawLifted(renderTarget, viewport);
        }
        return;
    }

    const auto it = m_states.find(w);
    if (it != m_states.end()) {
        applyTransform(w, it->second, data);
    }

    Effect::paintWindow(renderTarget, viewport, w, mask, deviceRegion, data);

    drawCaption(renderTarget, viewport, w);

    if (w == m_liftAnchor) {
        drawLifted(renderTarget, viewport);
    }
}

bool ThumbnailBloomEffect::isLifted(EffectWindow *w) const
{
    return std::ranges::find(m_lifted, w) != m_lifted.end();
}

void ThumbnailBloomEffect::drawLifted(
    const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    if (!m_liftPending) {
        return;
    }
    m_liftPending = false;

    // Read once for the whole set rather than per outline: building a
    // KColorScheme means reading and computing a whole palette, and every
    // thumbnail here draws its outline in the same colour anyway.
    const QColor outline = outlineColor();

    // Least enlarged first: the set is ordered that way, so the thumbnail the
    // pointer is growing ends up over the ones it is leaving behind.
    for (EffectWindow *w : m_lifted) {
        const auto it = m_states.find(w);
        if (it == m_states.end()) {
            continue;
        }

        // The region is the clip of the draw, and it has to be the damage of the
        // whole pass. The region the anchor was painted with is clipped to that
        // window's own area for an opaque window, which cuts the thumbnail down to
        // the part overlapping it; an infinite region has the opposite problem, since
        // painting outside the damage blends the translucent parts of the thumbnail
        // over pixels that were never cleared, so the shadow darkens frame by frame.
        WindowPaintData data;
        applyTransform(w, it->second, data);
        effects->drawWindow(renderTarget, viewport, w,
            PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT, m_paintRegion, data);

        // The caption of a lifted thumbnail follows it here, so that it ends up
        // over the thumbnail rather than under it, like every other one does.
        drawCaption(renderTarget, viewport, w);

        drawOutline(renderTarget, viewport, w, it->second, outline);
    }
}

void ThumbnailBloomEffect::drawCaption(
    const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w)
{
    const auto it = m_states.find(w);
    if (it == m_states.end()) {
        return;
    }

    // Kept from the relayout that placed the click target rather than looked up
    // here: this runs for every bloomed window of every frame, and the lookup
    // walks the internal windows, of which the effect itself makes two per
    // thumbnail. The pointer empties itself if the window goes away behind the
    // effect's back, and the lookup is done again then rather than the caption
    // being dropped for the frame.
    BloomState &state = it->second;
    if (!state.overlayWindow && state.overlay) {
        state.overlayWindow = effects->findWindow(state.overlay.get());
    }

    EffectWindow *overlay = state.overlayWindow;
    if (!overlay || !overlay->isVisible()) {
        return;
    }

    // Drawn untransformed and where it is: the click target already sits on the
    // resting rectangle of the thumbnail, so all this changes is the moment of
    // the draw. The region is the damage of the whole pass, for the same reason
    // as in drawLifted(): the region of the window just painted is clipped to
    // that window.
    WindowPaintData data;
    effects->drawWindow(renderTarget, viewport, overlay,
        PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT, m_paintRegion, data);
}

void ThumbnailBloomEffect::drawOutline(const RenderTarget &renderTarget,
    const RenderViewport &viewport, EffectWindow *w, const BloomState &state,
    const QColor &color) const
{
    const QRectF rect = state.rect.current;
    if (!effects->isOpenGLCompositing() || rect.isEmpty()) {
        return;
    }

    // The outline is turned with the thumbnail, through the very map its pixels
    // go through: same angle, same direction, same frame, so the two cannot drift
    // apart however the bend is animated. The inset is taken before the map and
    // not after, which is what makes the border thinner where the thumbnail
    // recedes, as the frame of a turned surface has to be.
    const qreal width = std::min(outlineWidth, std::min(rect.width(), rect.height()) / 3.0);
    const QTransform transform = stateBend(w, state, rect);

    // The projection matrix of the viewport orthos over the render rect scaled by
    // the output scale, so the vertices are logical screen coordinates multiplied
    // by that scale (device pixels, but with the origin of the whole logical
    // space, not of the output); on a screen scaled by 1 the two are the same, on
    // any other one they are not.
    const qreal scale = viewport.scale();
    const auto corners = [&](const QRectF &box) {
        return BendQuad { transform.map(box.topLeft()) * scale,
            transform.map(box.topRight()) * scale, transform.map(box.bottomRight()) * scale,
            transform.map(box.bottomLeft()) * scale };
    };
    const BendQuad outer = corners(rect);
    const BendQuad inner = corners(rect.adjusted(width, width, -width, -width));

    // One trapezoid per edge, between the outer corners and the inner ones.
    std::vector<QVector2D> vertices;
    vertices.reserve(24);
    for (size_t i = 0; i < outer.size(); ++i) {
        const size_t next = (i + 1) % outer.size();
        appendQuad(vertices, { outer[i], outer[next], inner[next], inner[i] });
    }

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setVertices(vertices);

    ShaderBinder binder(ShaderTrait::UniformColor | ShaderTrait::TransformColorspace);
    GLShader *shader = binder.shader();
    shader->setUniform(
        GLShader::Mat4Uniform::ModelViewProjectionMatrix, viewport.projectionMatrix());
    shader->setUniform(GLShader::ColorUniform::Color, color);
    shader->setColorspaceUniforms(
        ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

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

void ThumbnailBloomEffect::paintScreen(const RenderTarget &renderTarget,
    const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    // Nothing is painted here; this only keeps hold of the damage of the pass for
    // the thumbnail that is stamped from inside the window pass, which is handed
    // a per-window region instead.
    m_paintRegion = deviceRegion;

    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
}

void ThumbnailBloomEffect::postPaintScreen()
{
    // The next frame of the animation is asked for here, over the ground the
    // thumbnails are moving across rather than over the whole screen; its own
    // prePaintScreen() widens that damage to wherever they have got to by then.
    // Nothing is asked for on behalf of the lifted thumbnails: they are drawn
    // again whenever a pass happens at all, and one that has come to rest needs
    // no pass of its own.
    if (m_animating && !m_dirty.isEmpty()) {
        effects->addRepaint(Region(m_dirty));
    }

    Effect::postPaintScreen();
}

bool ThumbnailBloomEffect::isActive() const { return !m_states.empty(); }

int ThumbnailBloomEffect::requestedEffectChainPosition() const { return 50; }

} // namespace ThumbnailBloom

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(
    ThumbnailBloom::ThumbnailBloomEffect, "metadata.json", return true;, return false;)

#include "thumbnailbloom.moc"
