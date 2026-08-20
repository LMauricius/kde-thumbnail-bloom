/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailbloom.h"
#include "thumbnailbloomconfig.h"
#include "thumbnailoverlay.h"

#include <core/output.h>
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
            effects->addRepaint(RectF(it->second.current.adjusted(-1, -1, 1, 1)));
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
            retarget(w, QRectF(w->frameGeometry()));
        }
    }
}

void ThumbnailBloomEffect::retarget(EffectWindow *w, const QRectF &target)
{
    const auto [it, inserted] = m_states.try_emplace(w);
    BloomState &state = it->second;

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

void ThumbnailBloomEffect::forget(EffectWindow *w)
{
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

    state.overlay->setGeometry(state.current.toAlignedRect());
    state.overlay->show();
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

    for (EffectWindow *w : settledBack) {
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
    const auto it = m_states.find(w);
    if (it != m_states.end()) {
        const QRectF natural = w->frameGeometry();
        const QRectF &current = it->second.current;
        if (natural.width() > 0 && natural.height() > 0) {
            // Scaling happens around the window's top left corner, so the
            // translation is expressed in unscaled screen coordinates.
            data.setScale(QVector2D(current.width() / natural.width(), current.height() / natural.height()));
            data.setXTranslation(current.x() - natural.x());
            data.setYTranslation(current.y() - natural.y());
        }
    }

    Effect::paintWindow(renderTarget, viewport, w, mask, deviceRegion, data);
}

void ThumbnailBloomEffect::postPaintScreen()
{
    if (m_animating) {
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
