/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "bloomlayout.h"

#include <effect/effect.h>
#include <effect/timeline.h>

#include <QRectF>
#include <QTimer>

#include <memory>
#include <unordered_map>

namespace ThumbnailBloom
{

class ThumbnailOverlay;

/*!
 * Shows covered inactive windows as thumbnails on the nearest free space.
 *
 * Windows are never really moved or resized: they are painted scaled down and
 * translated, and a transparent click target (ThumbnailOverlay) is put on top
 * of each finished thumbnail so that it can be clicked into focus.
 */
class ThumbnailBloomEffect : public KWin::Effect
{
    Q_OBJECT

public:
    ThumbnailBloomEffect();
    ~ThumbnailBloomEffect() override;

    void reconfigure(ReconfigureFlags flags) override;

    void prePaintScreen(KWin::ScreenPrePaintData &data) override;
    void prePaintWindow(KWin::RenderView *view, KWin::EffectWindow *w, KWin::WindowPrePaintData &data) override;
    void paintScreen(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport, int mask, const KWin::Region &deviceRegion, KWin::LogicalOutput *screen) override;
    void paintWindow(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport, KWin::EffectWindow *w, int mask, const KWin::Region &deviceRegion, KWin::WindowPaintData &data) override;
    void postPaintScreen() override;

    bool isActive() const override;
    int requestedEffectChainPosition() const override;

private:
    /*!
     * Everything the effect keeps around for one bloomed window: where its
     * thumbnail travels from and to, and its click target.
     */
    struct BloomState
    {
        QRectF base; //!< rectangle the layout asked for, before any hover growth
        QRectF from; //!< rectangle the running animation started at
        QRectF to; //!< rectangle the animation ends at
        QRectF current; //!< rectangle used by the current frame
        bool hovered = false; //!< whether the pointer is on the thumbnail
        KWin::TimeLine timeline;
        std::unique_ptr<ThumbnailOverlay> overlay;
    };

    // --- state handling ---

    /*! Rebuilds the layout of every screen and retargets the animations. */
    void relayout();
    /*! Queues a relayout for the next event loop pass, coalescing bursts of changes. */
    void scheduleRelayout();
    /*! Starts or retargets the animation of \a w towards \a base, grown if hovered. */
    void retarget(KWin::EffectWindow *w, const QRectF &base);
    /*! Marks the topmost settled thumbnail under \a pos as hovered, and the rest as not. */
    void updateHover(const QPointF &pos);
    /*! Marks the thumbnail of \a w as hovered or not and animates it accordingly. */
    void setHovered(KWin::EffectWindow *w, bool hovered);
    /*! Drops \a w's state, hiding its click target right away. */
    void forget(KWin::EffectWindow *w);
    /*! Applies the thumbnail transformation of \a state to \a data. */
    void applyTransform(KWin::EffectWindow *w, const BloomState &state, KWin::WindowPaintData &data) const;
    /*! Draws the lifted thumbnail, if it has not been drawn in this pass yet. */
    void drawLifted(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport);
    /*! Keeps the click target of \a w in sync with its thumbnail. */
    void updateOverlay(KWin::EffectWindow *w, BloomState &state);

    // --- window classification ---

    /*! Whether \a w takes part in the layout at all (as thumbnail or as obstacle). */
    bool isRelevant(KWin::EffectWindow *w) const;
    /*! Whether \a w may be turned into a thumbnail, honouring the settings. */
    bool isEligible(KWin::EffectWindow *w, const QSet<KWin::EffectWindow *> &parents) const;
    /*! Whether \a w covers its whole maximize area (fullscreen counts as maximized). */
    bool isMaximized(KWin::EffectWindow *w) const;

    // --- connections ---

    /*! Connects the window signals that invalidate the layout. */
    void watch(KWin::EffectWindow *w);

    std::unordered_map<KWin::EffectWindow *, BloomState> m_states;
    QTimer m_relayoutTimer;
    std::chrono::milliseconds m_animationDuration;
    LayoutOptions m_layoutOptions;
    KWin::Region m_paintRegion; //!< device region the current pass repaints
    KWin::EffectWindow *m_liftedWindow = nullptr; //!< thumbnail drawn above the other windows
    KWin::EffectWindow *m_liftAnchor = nullptr; //!< window it is drawn right after
    bool m_liftPending = false; //!< whether it still has to be drawn in this pass
    bool m_skipKeepAbove = true;
    bool m_skipMaximized = true;
    bool m_skipParents = true;
    bool m_skipChildren = false;
    bool m_animating = false;
};

} // namespace ThumbnailBloom
