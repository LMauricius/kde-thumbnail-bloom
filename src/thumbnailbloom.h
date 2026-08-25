/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "bloomlayout.h"
#include "inputfilters.h"

#include <effect/effect.h>
#include <effect/offscreeneffect.h>
#include <effect/timeline.h>

#include <QHash>
#include <QPointer>
#include <QRegion>
#include <QRectF>
#include <QTimer>
#include <QTransform>

#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>

class QWindow;

namespace ThumbnailBloom {

class OverlayWindow;
class ThumbnailOverlay;

/*!
 * Shows covered inactive windows as thumbnails on the nearest free space.
 *
 * Windows are never really moved or resized: they are painted scaled down and
 * translated, and a transparent click target (ThumbnailOverlay) is put on top
 * of each finished thumbnail so that it can be clicked into focus.
 *
 * It is an OffscreenEffect because of the 3D bend: a resting thumbnail is drawn
 * turned away from the viewer, and nothing in WindowPaintData can express that.
 * A window redirected into a texture is deformed vertex by vertex instead, which
 * is what apply() does.
 */
class ThumbnailBloomEffect : public KWin::OffscreenEffect
{
    Q_OBJECT

public:
    ThumbnailBloomEffect();
    ~ThumbnailBloomEffect() override;

    void reconfigure(ReconfigureFlags flags) override;

    void prePaintScreen(KWin::ScreenPrePaintData &data) override;
    void prePaintWindow(
        KWin::RenderView *view, KWin::EffectWindow *w, KWin::WindowPrePaintData &data) override;
    void paintScreen(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        int mask, const KWin::Region &deviceRegion, KWin::LogicalOutput *screen) override;
    void paintWindow(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        KWin::EffectWindow *w, int mask, const KWin::Region &deviceRegion,
        KWin::WindowPaintData &data) override;
    void postPaintScreen() override;

    bool isActive() const override;
    int requestedEffectChainPosition() const override;

private:
    /*!
     * One animated value: where the running trip started, where it ends, and
     * what the current frame uses. Pure data; the TimeLine driving the progress
     * lives in BloomState, one for all four channels.
     */
    template <typename T>
    struct Animated
    {
        T from {}; //!< value the running animation started at
        T to {}; //!< value the animation ends at
        T current {}; //!< value used by the current frame

        /*! Puts the channel at \a value with no trip in flight. */
        void snap(const T &value) { from = to = current = value; }
        /*! Starts a fresh trip from wherever the channel is now towards \a target. */
        void restart(const T &target)
        {
            from = current;
            to = target;
        }
        /*! Linear blend of from and to at \a progress into current. */
        void interpolate(qreal progress);
    };

    /*!
     * Everything the effect keeps around for one bloomed window: where its
     * thumbnail travels from and to, and its click target.
     */
    struct BloomState
    {
        QRectF base; //!< rectangle the layout asked for, before any hover growth
        QRectF thumbBase; //!< resting rectangle of the last thumbnail, kept on the way home
        Animated<QRectF> rect; //!< rectangle the thumbnail is painted in
        QRectF painted; //!< screen area the last frame drew the thumbnail into
        Animated<qreal> opacity; //!< thumbnail opacity, 1.0 when hovered or at home
        Animated<qreal> caption; //!< caption opacity the click target paints with
        Animated<qreal> bend; //!< bend strength, 0 flat, 1 full angle
        bool redirected
            = false; //!< whether the window is being painted through an offscreen texture
        bool hovered = false; //!< whether the pointer is on the thumbnail
        bool overBackdrop
            = false; //!< whether the thumbnail is drawn over a backdrop stacked above its window
        QRegion
            hitRegion; //!< part of base left uncovered by system elements, in screen coordinates
        KWin::TimeLine timeline;
        std::unique_ptr<ThumbnailOverlay> overlay;
        QPointer<KWin::EffectWindow>
            overlayWindow; //!< the click target as the scene knows it, while it is shown
        std::unique_ptr<OverlayWindow>
            shield; //!< swallows the input the vacated real geometry would still get
    };

    /*!
     * One set of thumbnails drawn out of turn, and where in the pass that
     * happens.
     *
     * The lifted thumbnails come in two of these, so that only the ones the
     * pointer is animating end up over the active window; see updateLift().
     */
    struct LiftGroup
    {
        std::vector<KWin::EffectWindow *> windows; //!< the thumbnails, least enlarged first
        KWin::EffectWindow *anchor
            = nullptr; //!< window they are drawn right after, possibly one of them
        bool pending = false; //!< whether they still have to be drawn in this pass
    };

    // --- state handling ---

    /*! Rebuilds the layout of every screen and retargets the animations. */
    void relayout();
    /*!
     * Returns every window one of \a relevant is transient for, which is what
     * the "skip parents" setting works on.
     */
    QSet<KWin::EffectWindow *> transientParents(
        const std::vector<KWin::EffectWindow *> &relevant) const;
    /*!
     * Retargets every placed window towards its thumbnail, sends the rest of
     * the bloomed ones home, and refreshes the shields and the hover, in that
     * order.
     */
    void applyPlacements(const QHash<KWin::LogicalOutput *, QList<LayoutWindow>> &perScreen);
    /*! Queues a relayout for the next event loop pass, coalescing bursts of changes. */
    void scheduleRelayout();
    /*! Starts or retargets the animation of \a w towards \a base, grown if hovered. */
    void retarget(KWin::EffectWindow *w, const QRectF &base);
    /*! Returns the bloomed window whose window menu is open, if any. */
    KWin::EffectWindow *menuOwner() const;
    /*! Opens the window menu of \a w at \a pos and keeps its thumbnail focused. */
    void openWindowMenu(KWin::EffectWindow *w, const QPointF &pos);
    /*! Marks the topmost thumbnail whose exposed area holds \a pos as hovered, and the rest as not. */
    void updateHover(const QPointF &pos);
    /*! Returns the thumbnail the pointer at \a pos is usably on, or nullptr. */
    KWin::EffectWindow *thumbnailUnder(const QPointF &pos) const;
    /*! Marks the thumbnail of \a w as hovered or not and animates it accordingly. */
    void setHovered(KWin::EffectWindow *w, bool hovered);
    /*!
     * Activates \a w and hands it to the interactive move, with \a pos held.
     *
     * The window is put where its thumbnail is before the move begins, so that
     * it comes out from under the finger or the pointer instead of jumping back
     * to wherever it really was. \a touchId is the point driving the move, or
     * -1 when the pointer is.
     */
    void startThumbnailMove(KWin::EffectWindow *w, const QPointF &pos, qint32 touchId);
    /*! Drops \a w's state, hiding its click target right away. */
    void forget(KWin::EffectWindow *w);
    /*!
     * Bends the offscreen texture of \a window, which is what makes it look 3D.
     *
     * The quads arrive in window coordinates, with the frame geometry at the
     * origin, and are mapped through the projective transform that takes the
     * frame rectangle onto its bent corners. They are subdivided first: the
     * texture coordinates of a quad are interpolated linearly across it, so a
     * single quad would be textured as if it were flat, and only cutting it into
     * a grid small enough makes the pixels follow the perspective.
     */
    void apply(KWin::EffectWindow *window, int mask, KWin::WindowPaintData &data,
        KWin::WindowQuadList &quads) override;
    /*! Redirects \a w into an offscreen texture, or stops doing so, as \a redirected asks. */
    void setRedirected(KWin::EffectWindow *w, BloomState &state, bool redirected);
    /*!
     * The bend of \a state applied over \a rect: the configured angle scaled by
     * the animated strength, leaning towards the window's real place. \a rect is
     * the frame at the origin for the pixels (apply()) and the on-screen
     * rectangle for the outline (drawOutline()); the direction is taken from the
     * on-screen rectangle either way, so the two cannot drift apart.
     */
    QTransform stateBend(KWin::EffectWindow *w, const BloomState &state, const QRectF &rect) const;
    /*!
     * Returns everything the thumbnail of \a w puts on the screen as \a state
     * stands, which is the area a repaint has to cover for it to come out whole.
     *
     * Larger than the rectangle the layout works with on two counts: the window
     * is drawn with its shadow, which reaches outside its frame, and the bend
     * carries that shadow through the very projective map the pixels go through,
     * which can take a corner outside the frame further out still. Padding the
     * plain rectangle would not do; the map has to be applied.
     */
    QRectF paintedArea(KWin::EffectWindow *w, const BloomState &state) const;
    /*! Applies the thumbnail transformation of \a state to \a data. */
    void applyTransform(
        KWin::EffectWindow *w, const BloomState &state, KWin::WindowPaintData &data) const;
    /*!
     * Advances every timeline one frame, interpolates the four channels, and
     * pushes the caption opacity into the click targets. Sets m_animating and
     * returns the windows whose thumbnails have arrived back at their real
     * geometry, for the caller to forget.
     */
    std::vector<KWin::EffectWindow *> advanceAnimations(KWin::ScreenPrePaintData &data);
    /*!
     * Rebuilds both lift groups for \a screen (least enlarged first) and picks
     * the anchor each one follows. Only the thumbnails of that screen take part,
     * since a pass paints one screen and the anchor has to be painted in it.
     */
    void updateLift(KWin::LogicalOutput *screen);
    /*! Whether \a w is one of the thumbnails drawn above the windows covering them. */
    bool isLifted(KWin::EffectWindow *w) const;
    /*! Whether \a w is one of \a group. */
    static bool isLifted(const LiftGroup &group, KWin::EffectWindow *w);
    /*! Draws \a group, least enlarged first, if it is still due in this pass. */
    void drawLifted(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        LiftGroup &group);
    /*!
     * Draws the hover outline just inside the thumbnail of \a w, in logical
     * screen coordinates.
     *
     * The whole \a state is taken rather than a rectangle, because the outline
     * is turned by the same bend as the pixels of the thumbnail and needs the
     * strength this frame is drawn with. \a color is read once for the whole
     * set of them, a palette being too much to build per outline.
     */
    void drawOutline(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        KWin::EffectWindow *w, const BloomState &state, const QColor &color) const;
    /*! Puts the click target of \a w on its resting rectangle, or hides it. */
    void updateOverlay(KWin::EffectWindow *w, BloomState &state);
    /*!
     * Draws the caption of \a w, which is to say its click target, if it has one.
     *
     * Called right after the thumbnail of \a w has been drawn, which is what
     * puts the caption at the depth of the thumbnail rather than at the top of
     * the screen, where the layer of an internal window would otherwise keep it.
     */
    void drawCaption(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        KWin::EffectWindow *w);
    /*! Puts a shield on the part of every bloomed window that would still take input. */
    void updateShields();
    /*!
     * Puts \a state's shield over the part of \a frame not in \a covered, and
     * returns that part. An empty result places nothing and leaves any old
     * shield alone; updateShields() drops the stale ones in one place.
     */
    QRegion placeShield(BloomState &state, const QRect &frame, const QRegion &covered);

    // --- window classification ---

    /*! Whether \a w takes part in the layout at all (as thumbnail or as obstacle). */
    bool isRelevant(KWin::EffectWindow *w) const;
    /*! Whether the settings exempt \a w from the effect, so that it neither blooms nor makes others bloom. */
    bool isIgnored(KWin::EffectWindow *w, const QSet<KWin::EffectWindow *> &parents) const;
    /*!
     * Whether \a w may be turned into a thumbnail, \a ignored saying whether the
     * settings exempt it. That half is worked out by the caller, which needs the
     * answer for itself anyway.
     */
    bool isEligible(KWin::EffectWindow *w, bool ignored) const;
    /*! Whether \a w covers its whole maximize area (fullscreen counts as maximized). */
    bool isMaximized(KWin::EffectWindow *w) const;
    /*!
     * Whether \a w is a backdrop: a maximized window the thumbnails are shown
     * over rather than squeezed around.
     *
     * A maximized window leaves no free space at all, so the whole effect would
     * come to nothing on that screen. It is therefore taken out of the placement
     * entirely (it blocks nothing, wherever it sits in the stack) while it keeps
     * hiding what it covers, so the windows behind it are exactly the ones that
     * bloom out over it. Only on a screen updateBackdropScreens() allowed it on.
     */
    bool isBackdrop(KWin::EffectWindow *w) const;
    /*!
     * Recomputes the screens the backdrop exception applies to, \a ignored
     * telling for each window of \a relevant whether the settings exempt it.
     *
     * The topmost window of a screen is what that screen is being used for, and
     * a maximized one there is what the user asked to see: nothing may be laid
     * over it, so that screen gets no backdrops at all. Windows the settings
     * exempt are passed over on the way to it, a maximized one excepted, since
     * "skip maximized" exempts exactly the windows the question is about. Every
     * screen is answered on its own, so a maximized window in front on one of
     * them holds back nothing on the others. The active window has the last word
     * on the screen it is on: while it is the maximized one, that screen gets no
     * backdrops whatever is stacked over it.
     */
    void updateBackdropScreens(
        const std::vector<KWin::EffectWindow *> &relevant, const std::vector<bool> &ignored);
    /*! Whether \a w is one of the effect's own click targets or shields. */
    bool isOwnOverlay(KWin::EffectWindow *w) const;
    /*! Whether \a w is one of the effect's own click targets, the surfaces the captions are painted on. */
    bool isCaptionTarget(KWin::EffectWindow *w) const;
    /*! Recomputes the area the system elements take away from the thumbnails. */
    void updateSystemRegion();

    // --- connections ---

    /*! Connects the window signals that invalidate the layout. */
    void watch(KWin::EffectWindow *w);

    std::unordered_map<KWin::EffectWindow *, BloomState> m_states;
    //! Every surface of the effect's own: the click targets and the shields.
    std::unordered_set<const QWindow *> m_ownOverlays;
    //! The click targets alone, the surfaces the captions are painted on.
    std::unordered_set<const QWindow *> m_captionTargets;
    QTimer m_relayoutTimer;
    std::chrono::milliseconds m_animationDuration { 250 };
    bool m_showIcons = true;
    bool m_showTitles = true;
    qreal m_thumbnailOpacity = 0.9; //!< opacity of a thumbnail that is not hovered
    qreal m_bendAngle
        = 15.0; //!< angle a resting thumbnail is turned by, in degrees; 0 keeps them flat
    LayoutOptions m_layoutOptions;
    QRegion m_systemRegion; //!< screen area covered by panels, popups and other system elements
    QSet<KWin::LogicalOutput *>
        m_backdropScreens; //!< screens whose maximized windows are backdrops, from the last layout
    ShieldFilter m_shieldFilter;
    TouchDragFilter m_touchDragFilter;
    KWin::Region m_paintRegion; //!< device region the current pass repaints
    QRegion m_dirty; //!< logical area the running animations have to repaint
    KWin::EffectWindow *m_menuOwner
        = nullptr; //!< window whose menu is open, kept focused meanwhile
    KWin::EffectWindow *m_menuPopup = nullptr; //!< the menu itself, watched for its closing
    LiftGroup m_liftedBelow; //!< the ones at rest, drawn no higher than the window covering them
    LiftGroup m_liftedAbove; //!< the ones being resized by a hover or a trip, drawn over the rest
    bool m_skipKeepAbove = true;
    bool m_skipOnAllDesktops = true;
    bool m_skipMaximized = true;
    bool m_skipParents = true;
    bool m_skipChildren = false;
    bool m_animating = false;
};

} // namespace ThumbnailBloom
