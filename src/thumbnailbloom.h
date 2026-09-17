/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "bloomlayout.h"
#include "inputfilters.h"

#include <effect/effect.h>
#include <effect/timeline.h>
#include <opengl/glframebuffer.h>
#include <opengl/glshader.h>
#include <opengl/gltexture.h>
#include <scene/item.h>
#include <scene/itemgeometry.h>

#include <QColor>
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

class ThumbnailCanvas;
class OverlayWindow;
class ThumbnailOverlay;

/*!
 * Shows covered inactive windows as thumbnails on the nearest free space.
 *
 * Windows are never really moved or resized: they are painted scaled down and
 * translated, and a transparent click target (ThumbnailOverlay, one per screen,
 * masked to the thumbnails) lies over them so that they can be clicked into
 * focus. The frame
 * around a thumbnail and the caption on it are painted by one window of their
 * own (ThumbnailCanvas), which is the only surface of the effect that is ever
 * drawn.
 *
 * Every bloomed window is drawn through a store of its own (drawWindow()): a
 * texture holding the window at its real size, with a mip chain over it. That
 * is what lets the thumbnail be bent, which nothing in WindowPaintData can
 * express, and what keeps a thumbnail from aliasing, a downscale reading one
 * texel out of the many each of its pixels covers.
 */
class ThumbnailBloomEffect : public KWin::Effect
{
    Q_OBJECT

public:
    ThumbnailBloomEffect();
    ~ThumbnailBloomEffect() override;

    void reconfigure(ReconfigureFlags flags) override;

    void prePaintScreen(KWin::ScreenPrePaintData &data) override;
    void prePaintWindow(
        KWin::RenderView *view, KWin::EffectWindow *w, KWin::WindowPrePaintData &data) override;
    void paintWindow(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        KWin::EffectWindow *w, int mask, const KWin::Region &deviceRegion,
        KWin::WindowPaintData &data) override;
    /*!
     * Draws \a w out of its own offscreen store, bent and placed as \a data asks.
     *
     * The store is what the bend and the filtering both need: a window has to be
     * a texture before its vertices can be moved one by one, and it has to carry
     * a mip chain before it can be drawn smaller than it is without aliasing.
     * A window with no store of its own (no state, no OpenGL, no room for the
     * texture) is handed straight to the ordinary path and looks as it always
     * did.
     */
    void drawWindow(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        KWin::EffectWindow *w, int mask, const KWin::Region &deviceRegion,
        KWin::WindowPaintData &data) override;
    void postPaintScreen() override;

    bool isActive() const override;
    /*!
     * Whether the screen has to be composited rather than scanned out directly.
     *
     * A window handed to the scanout hardware is shown on its own, with nothing
     * drawn over it, so every thumbnail on that screen would disappear. The
     * effect therefore holds the screen back for as long as it has a thumbnail
     * to draw.
     */
    bool blocksDirectScanout() const override;
    int requestedEffectChainPosition() const override;

protected:
    /*!
     * Watches the application object for a colour scheme change, which in Qt 6
     * is the only place one is announced.
     *
     * The two colours a frame is drawn between are read once and kept, a
     * KColorScheme being far too much to build on every frame of every pass; this
     * is what tells the effect the answer has moved on.
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /*!
     * One animated value: where the running trip started, where it ends, and
     * what the current frame uses. Pure data; the TimeLine driving the progress
     * lives in BloomState, one for all of its channels.
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
     * Which trip a thumbnail is on, of the two the paint pass draws out of the
     * stacking order. Every other move it makes is an ordinary one.
     */
    enum class Lift
    {
        None, //!< laid out like any other thumbnail, wherever it is heading
        Hover, //!< the pointer's growth, and the way back down from it
        Home, //!< the picked thumbnail travelling back to its own window
        Dive, //!< the whole screen's thumbnails shrinking into the burst point
    };

    /*!
     * Everything the effect keeps around for one bloomed window: where its
     * thumbnail travels from and to, what it answers for, and its store.
     */
    struct BloomState
    {
        QRectF base; //!< rectangle the layout asked for, before any hover growth
        QRectF thumbBase; //!< resting rectangle of the last thumbnail, kept on the way home
        //! Rectangle the thumbnail settles at: the grown one while the pointer is on it.
        QRectF hoverRect;
        Animated<QRectF> rect; //!< rectangle the thumbnail is painted in
        QRectF painted; //!< screen area the last frame drew the thumbnail into
        Animated<qreal> opacity; //!< thumbnail opacity, 1.0 when hovered or at home
        Animated<qreal> caption; //!< caption opacity the store paints with
        Animated<qreal> bend; //!< bend strength, 0 flat, 1 full angle
        //! How far the outline is towards its hover weight, 1 while the pointer is on the thumbnail.
        Animated<qreal> highlight;
        //! Whether the window is drawn through an offscreen store of its own.
        bool snapshot = false;
        //! The window at its real size, mip chained; made on the first draw that needs it.
        std::unique_ptr<KWin::GLTexture> texture;
        std::unique_ptr<KWin::GLFramebuffer> fbo; //!< what the store is drawn into
        bool stale = true; //!< whether the window has changed since the store was drawn
        //! Keeps the scene rendering the window while the store draws from it.
        KWin::ItemEffect item;
        bool hovered = false; //!< whether the pointer is on the thumbnail
        //! Whether a click of the window's own has landed on the thumbnail since the pointer arrived.
        bool clicked = false;
        Lift lift = Lift::None; //!< the trip the thumbnail is on, settled by retarget()
        bool overBackdrop
            = false; //!< whether the thumbnail is drawn over a backdrop stacked above its window
        //! Whether the window is shown where it really is because something it put up holds a
        //! grab. Its placement still stands in the layout; the state is kept for as long as the
        //! grab lasts and lifts the window over the ones covering it. See showInPlace().
        bool blocked = false;
        bool diving
            = false; //!< whether the thumbnail is shrinking into the burst point of its screen
        bool homing
            = false; //!< whether the running trip is the one back to the window's real geometry
        //! What the click target claims, in screen coordinates: the resting rectangle, or the
        //! whole enlarged one once a click has landed on it, either way minus what covers it
        //! (a panel, a popup, or the enlarged rectangle of the thumbnail holding the pointer).
        QRegion hitRegion;
        //! Centre of the window's real geometry the frame in the store was last bent towards.
        QPointF bendOrigin;
        //! Which reading of the outline colours the store was last painted with.
        quint32 outlineSerial = 0;
        //! Whether the channels have moved on since the store was last handed them.
        bool canvasStale = true;
        KWin::TimeLine timeline;
        //! Store the frame and the caption of the thumbnail are painted in, and the one surface
        //! of the effect's own that is drawn at all.
        std::unique_ptr<ThumbnailCanvas> canvas;
        QPointer<KWin::EffectWindow>
            canvasWindow; //!< that store as the scene knows it, while it is shown
    };

    /*!
     * The two input windows of one screen: the click target over its thumbnails
     * and the shield over the real places of its bloomed windows.
     *
     * One pair per screen rather than one per thumbnail. Both are kept at the
     * geometry of the screen and only their masks move, which KWin reads live in
     * the hit test and which costs no buffer: a window resized on every relayout
     * is a buffer reallocated, cleared, uploaded and damaged for every one of
     * them, and a shield is as large as the window it stands in for.
     */
    struct ScreenInput
    {
        std::unique_ptr<ThumbnailOverlay> target; //!< takes the gestures of every thumbnail
        std::unique_ptr<OverlayWindow> shield; //!< swallows the input the vacated geometry would get
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
     * Takes the whole effect off the screen at once, dropping every thumbnail
     * where it stands rather than sending it home.
     *
     * For as long as a full screen effect is running, that effect is what the
     * session looks like. It animates each window from the rectangle the window
     * really occupies, which a thumbnail never changes, and there is no way for
     * one effect to tell another where it is painting; so the bloom cannot be
     * met half way and gets out of the way instead.
     */
    void standDown();
    /*!
     * Returns every window one of \a relevant is transient for, which is what
     * the "skip parents" setting works on.
     */
    QSet<KWin::EffectWindow *> transientParents(
        const std::vector<KWin::EffectWindow *> &relevant) const;
    /*!
     * Returns the windows whose input something they put up has taken over: the
     * owners, however far up the transient chain, of every popup holding a grab.
     *
     * Such a popup is drawn against the real surface of its window rather than
     * against the thumbnail, and the grab leaves the thumbnail inert, so the
     * window shows none of its bloom while one is up; see applyPlacements().
     */
    QSet<KWin::EffectWindow *> blockedWindows() const;
    /*!
     * Retargets every placed window towards its thumbnail, sends the rest of
     * the bloomed ones home, and refreshes the shields and the hover, in that
     * order. The \a blocked ones are counted among the rest, whatever the layout
     * handed them.
     */
    void applyPlacements(const QHash<KWin::LogicalOutput *, QList<LayoutWindow>> &perScreen,
        const QSet<KWin::EffectWindow *> &blocked);
    /*!
     * Takes the bloom off \a w and shows it where it really is, for as long as
     * something it put up holds a grab.
     *
     * The click target and the shield go with the thumbnail, so the window has
     * its own input back, grab and all. Its placement is left standing in the
     * layout, so that the thumbnails around it hold still and it blooms back
     * into the very same rectangle once the grab is over. The state is kept
     * rather than dropped at the end of the trip: it is what draws the window
     * over the ones covering it, which is the only way it and what it put up are
     * seen together.
     */
    void showInPlace(KWin::EffectWindow *w);
    /*! Queues a relayout for the next event loop pass, coalescing bursts of changes. */
    void scheduleRelayout();
    /*!
     * Starts or retargets the animation of \a w towards \a placement, grown if
     * hovered and rounded to whole physical pixels either way, since that is
     * what the thumbnail comes to rest on.
     *
     * An empty \a placement is the dive: the thumbnail is not heading anywhere it
     * could be seen, it is shrinking into the point its whole screen collapses
     * to, and the state is dropped once it arrives. \a burst is the other half
     * of the same gesture: the trip then starts at that point, at no size and
     * fully transparent, rather than wherever the thumbnail happens to be.
     */
    void retarget(KWin::EffectWindow *w, const QRectF &placement, const QPointF *burst = nullptr);
    /*!
     * The point the thumbnails of \a screen burst out of and dive back into:
     * the centre of the last window that spoke for that screen without being
     * maximized, or the middle of its work area while there was none.
     */
    QPointF burstPoint(KWin::LogicalOutput *screen) const;
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
     * Hovers the thumbnail of \a window because input was aimed at it.
     *
     * Asking the window for something through its thumbnail is the arrival
     * updateHover() would otherwise wait for, and waiting is not an option: a
     * wheel makes no motion event at all, and a button going down freezes the
     * hover for as long as it is held.
     */
    void engage(KWin::Window *window);
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
        KWin::WindowQuadList &quads);
    /*!
     * Gives \a w a store of its own, or takes it away, as \a wanted asks.
     *
     * The texture is the size of the whole window and there is one per bloomed
     * window, so it is dropped the moment it stops being drawn from rather than
     * kept against the next time. Wanted for every thumbnail, bend or no bend:
     * one is drawn smaller than its window either way.
     */
    void setSnapshot(KWin::EffectWindow *w, BloomState &state, bool wanted);
    /*!
     * Brings the store of \a state up to date with \a w and says whether there
     * is one to draw from.
     *
     * The window is drawn into it only when it has changed since the last time,
     * so a still thumbnail costs nothing at all per frame, and the mip chain is
     * built in the same breath: every level is made from the one above it, so
     * the whole chain costs a third of the draw that has just happened.
     */
    bool refreshSnapshot(KWin::EffectWindow *w, BloomState &state);
    /*!
     * The shader a thumbnail at rest is drawn with, built on the first draw
     * that wants it, or nothing at all if the scene cannot compile it.
     *
     * Kept by the effect rather than by the window: it holds no state of its
     * own, and one of it serves every thumbnail on every screen.
     */
    KWin::GLShader *filterShader();
    /*!
     * Draws the store of \a state where \a quads put it.
     *
     * What the scene does for an ordinary window, in the same terms: the same
     * uniforms, the same blending and the same clipping, with the store
     * standing in for the window's own texture.
     *
     * Which shader draws it is decided here. A thumbnail standing still, at its
     * resting rectangle or grown to the full of a hover, is worth the filtering
     * shader; one on a trip is drawn by the stock one, a single sample off the
     * mip chain, since nothing the filter works out can be made out on a moving
     * picture and every step of a trip repaints.
     */
    void paintSnapshot(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        KWin::EffectWindow *w, BloomState &state, const KWin::Region &deviceRegion,
        const KWin::WindowPaintData &data, const KWin::WindowQuadList &quads);
    /*!
     * The bend of \a state applied over \a rect: the configured angle scaled by
     * the animated strength, leaning towards the window's real place. \a rect is
     * the frame at the origin for the pixels (apply()) and the rectangle the
     * thumbnail is drawn on, in the coordinates of the frame store, for the
     * outline (refreshCanvas()); the direction is taken from the on-screen
     * rectangle either way, so the two cannot drift apart.
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
    /*!
     * Applies the thumbnail transformation of \a state to \a data.
     *
     * The picture is drawn thumbnailBleed larger than the rectangle of \a state
     * on every side, so that its edge runs under the frame instead of meeting
     * it. Nothing but the drawing grows by it.
     */
    void applyTransform(
        KWin::EffectWindow *w, const BloomState &state, KWin::WindowPaintData &data) const;
    /*!
     * Advances every timeline one frame, interpolates the channels and repaints
     * the store of every thumbnail with them. Fills m_moved and the
     * pending region of every screen with the ground the step covered, and
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
    /*!
     * Draws \a group, least enlarged first, if it is still due in this pass.
     *
     * \a deviceRegion is the region the anchor of the group was painted with,
     * which is the damage of the pass less whatever opaque windows above the
     * anchor cover: exactly the ground a thumbnail drawn after it can be seen
     * on.
     */
    void drawLifted(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        LiftGroup &group, const KWin::Region &deviceRegion);
    /*!
     * Draws the store of \a state: the frame around the thumbnail and the
     * caption on it, in a window of its own (ThumbnailCanvas) rather than
     * anything the effect stamps on the screen.
     *
     * Called right after the thumbnail itself, which is what gives both of them
     * the depth of that thumbnail rather than the top of the screen, where the
     * layer of an internal window would otherwise keep them: from there the
     * compositor covers the picture, the frame and the caption together, and
     * nothing has to be worked out from geometry. Over the thumbnail although
     * the line lies outside it: the shadow of a window is painted with the
     * thumbnail and reaches past the rectangle the frame goes on, so a line
     * drawn underneath would be tinted by it.
     *
     * refreshCanvas() has already put the store on the thumbnail and painted
     * into it at the size of this very frame, so all this normally does is draw
     * the window where it is. Should the store hold an older frame after all, it
     * is scaled from the rectangle it was painted around onto the one the
     * thumbnail is drawn at, so the drawing cannot lag behind the picture
     * whatever became of the paint. \a deviceRegion is the clip, the region the
     * thumbnail itself is painted with.
     */
    void drawCanvas(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        BloomState &state, const KWin::Region &deviceRegion);
    /*!
     * Works out what the thumbnail of \a w answers for (BloomState::hitRegion):
     * its resting rectangle, or the whole enlarged one once a click has landed
     * on it, less the system elements and the enlarged neighbour holding the
     * pointer. Empty for a window on its way home or into the burst point.
     *
     * Input and nothing else: the click target of the screen takes the union of
     * these as its mask (updateInputWindows()), and what is drawn on a thumbnail
     * belongs to its store (updateCanvas()).
     */
    void updateHitRegion(KWin::EffectWindow *w, BloomState &state);
    /*! Puts the store of \a w up with its caption, or takes it down. */
    void updateCanvas(KWin::EffectWindow *w, BloomState &state);
    /*!
     * Places the store of \a w and hands it this frame's corners, width, colour
     * and strength, along with how opaque the caption on it is.
     *
     * Every thumbnail is framed and the state's highlight channel decides only
     * how heavily: a thin caption-coloured line at rest, thickening and turning
     * to the focus colour under the pointer, so the hover changes the weight of
     * a frame that is already there instead of raising one out of transparency.
     * The store is kept at the largest rectangle the running trip draws and
     * moved onto the thumbnail every frame, and the line is painted into it at
     * the size it is drawn at, bent by the same map as the pixels of the
     * thumbnail. So nothing about it is ever scaled, and its width is the width
     * it asks for. It lies outside the thumbnail, which is why the store is a
     * margin larger than the rectangle it holds, and it is the line rather than
     * the thumbnail that is put on the physical pixel grid: the band it covers
     * is snapped and its width is a whole number of pixels, so it is drawn sharp
     * at every step of an animation. What that leaves between the line and the
     * picture is covered by thumbnailBleed. The corners handed over are the
     * inside of the frame, which the store fills outwards from and rounds on its
     * outer corners alone.
     *
     * The caption goes with the rectangle the thumbnail is drawn at and the size
     * it rests at: the store lays it out for the resting size and carries it
     * along a trip rather than leaving it at the destination, which is what
     * keeps it from having to be drawn again, shadows and all, on every frame.
     */
    void refreshCanvas(KWin::EffectWindow *w, BloomState &state);
    /*!
     * Places the click target and the shield of every screen: the first over the
     * hit regions of the thumbnails, the second over the part of every bloomed
     * window that would still take input. Hands the filter the same regions.
     */
    void updateInputWindows();
    /*!
     * Keeps \a window at \a screen's geometry with \a mask as its input region,
     * showing it on the way; hides it when the mask is empty, which is when
     * that screen has nothing of the kind.
     */
    void placeInputWindow(OverlayWindow *window, const QRect &screen, const QRegion &mask);
    /*!
     * Shows or hides \a window, one of the effect's own, without the stacking
     * change that makes running a layout pass over again: the effect's windows
     * are no input to the layout, and the pass that put them up has just run.
     */
    void setOwnWindowVisible(QWindow *window, bool visible);

    // --- window classification ---

    /*! Whether \a w takes part in the layout at all (as thumbnail or as obstacle). */
    bool isRelevant(KWin::EffectWindow *w) const;
    /*!
     * Whether the settings exempt \a w from the effect, so that it neither
     * blooms nor makes others bloom; \a maximized is isMaximized(w), worked out
     * once by the caller, since it costs a work area lookup and the layout asks
     * for it three times per window.
     */
    bool isIgnored(
        KWin::EffectWindow *w, bool maximized, const QSet<KWin::EffectWindow *> &parents) const;
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
     *
     * The same walk settles the burst as well. Which screens changed their mind
     * since the last layout is exactly which ones gained or lost every thumbnail
     * they had, since a screen out of the exception has its whole work area
     * blocked by that maximized window and can place nothing; those two sets go
     * to m_burstScreens and m_diveScreens. The window that speaks for a screen
     * without being maximized also leaves its centre behind as the point the
     * burst comes out of, which a maximized speaker then keeps rather than
     * replaces (see burstPoint()).
     */
    void updateBackdropScreens(const std::vector<KWin::EffectWindow *> &relevant,
        const std::vector<bool> &ignored, const std::vector<bool> &maximized);
    /*!
     * Whether \a w is one of the effect's own surfaces: a store, a click target
     * or a shield.
     *
     * Every one of them is left out of the pass at its own place in the stacking
     * order, where the layer of an internal window would put it over everything.
     * The stores are drawn again right after the thumbnail each one belongs to;
     * the other two are never drawn at all, taking input and nothing else, and
     * painting a transparent buffer over the damage of every frame is exactly
     * what they are spared that way.
     */
    bool isOwnOverlay(KWin::EffectWindow *w) const;
    /*! Recomputes the area the system elements take away from the thumbnails. */
    void updateSystemRegion();

    // --- connections ---

    /*! Connects the window signals that invalidate the layout. */
    void watch(KWin::EffectWindow *w);

    std::unordered_map<KWin::EffectWindow *, BloomState> m_states;
    std::unordered_map<KWin::LogicalOutput *, ScreenInput> m_input; //!< see ScreenInput
    //! Nonzero while one of the effect's own windows is being shown or hidden; see
    //! setOwnWindowVisible().
    int m_ownWindowChange = 0;
    QTimer m_relayoutTimer;
    std::chrono::milliseconds m_animationDuration { 250 };
    bool m_showIcons = true;
    bool m_showTitles = true;
    qreal m_thumbnailOpacity = 0.9; //!< opacity of a thumbnail that is not hovered
    //! Draws a thumbnail out of its store, averaging the texture over each pixel.
    std::unique_ptr<KWin::GLShader> m_filterShader;
    bool m_filterShaderBuilt = false; //!< whether building it has been tried at all
    qreal m_bendAngle
        = 15.0; //!< angle a resting thumbnail is turned by, in degrees; 0 keeps them flat
    LayoutOptions m_layoutOptions;
    QRegion m_systemRegion; //!< screen area covered by panels, popups and other system elements
    QSet<KWin::LogicalOutput *>
        m_backdropScreens; //!< screens whose maximized windows are backdrops, from the last layout
    //! Screens that took the backdrop exception back in the last layout: their thumbnails all burst out at once.
    QSet<KWin::LogicalOutput *> m_burstScreens;
    //! Screens that lost it: their thumbnails all dive into the one point at once.
    QSet<KWin::LogicalOutput *> m_diveScreens;
    //! Centre of the last non-maximized window that spoke for each screen; see burstPoint().
    QHash<KWin::LogicalOutput *, QPointF> m_screenFocus;
    //! Whether a layout has run at all, so that the first one bursts nothing.
    bool m_backdropsSettled = false;
    ShieldFilter m_shieldFilter;
    TouchDragFilter m_touchDragFilter;
    DragDropFilter m_dragDropFilter;
    QColor m_restOutline; //!< outline colour of a thumbnail at rest, read once per colour scheme
    QColor m_hoverOutline; //!< outline colour of a thumbnail under the pointer, ditto
    bool m_outlineDirty = true; //!< whether the two have to be read again
    //! Bumped whenever they are, so that a thumbnail at rest knows to repaint its frame once.
    quint32 m_outlineSerial = 0;
    /*!
     * The windows the last layout found relevant, which is what the paint pass
     * asks instead of working it out again.
     *
     * isRelevant() is some fifteen calls into the window, and the anchor walk
     * puts the question to every window of the session on every frame it draws.
     * The answer can only change with something that schedules a relayout, and
     * the pass in between draws exactly the arrangement that layout settled, so
     * the set is the right thing to read there. Nothing is ever dereferenced out
     * of it, so a window closed since is simply one the set no longer names.
     */
    std::unordered_set<KWin::EffectWindow *> m_relevantWindows;
    QRegion m_moved; //!< logical ground the animations covered in this pass
    /*!
     * Per screen, the logical ground that has moved since that screen last
     * painted, which is what the pass of that screen widens its damage by.
     *
     * Kept per screen because every one of them paints a pass of its own, at its
     * own rate, and the animations advance in each of those passes: a screen
     * that has not painted for a step or two has to erase the thumbnail where it
     * really drew it, not where the last pass of some other screen left it.
     */
    std::unordered_map<KWin::LogicalOutput *, QRegion> m_pending;
    KWin::EffectWindow *m_menuOwner
        = nullptr; //!< window whose menu is open, kept focused meanwhile
    KWin::EffectWindow *m_menuPopup = nullptr; //!< the menu itself, watched for its closing
    //! Where the pointer was the last time the hover was worked out, which is what says whether
    //! a hover is being arrived at or merely sat on; see updateHover().
    QPointF m_hoverPos;
    LiftGroup m_liftedBelow; //!< the ones at rest, drawn no higher than the window covering them
    LiftGroup m_liftedAbove; //!< the ones being resized by a hover or a trip, drawn over the rest
    bool m_skipKeepAbove = true;
    bool m_skipOnAllDesktops = true;
    bool m_skipMaximized = true;
    bool m_skipParents = true;
    bool m_skipChildren = false;
};

} // namespace ThumbnailBloom
