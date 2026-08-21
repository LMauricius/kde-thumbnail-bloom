/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "bloomlayout.h"

#include <effect/effect.h>
#include <effect/offscreeneffect.h>
#include <effect/timeline.h>
#include <input.h>

#include <QPointer>
#include <QRegion>
#include <QRectF>
#include <QTimer>

#include <memory>
#include <vector>
#include <unordered_map>

namespace ThumbnailBloom {

class OverlayWindow;
class ThumbnailOverlay;

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
 */
class ShieldFilter : public KWin::InputEventFilter
{
public:
    ShieldFilter();

    /*! Where one click target is, and whose window it shows. */
    struct Thumbnail
    {
        KWin::Window *window;
        QRegion region;
    };

    /*!
     * Sets what the filter works on, all in logical screen coordinates.
     *
     * \a shields is where the shields are, \a bloomed the windows they hide
     * from the input, and \a thumbnails the click targets: a thumbnail belongs
     * to its own window, so every button that means nothing on a thumbnail is
     * dropped there rather than handed to whatever it is painted over.
     */
    void setState(const QRegion &shields, const QSet<KWin::Window *> &bloomed,
        const QList<Thumbnail> &thumbnails);

    /*!
     * Whether \a window is hidden at \a pos by a window painted over it.
     *
     * A thumbnail is painted at the stacking position of the window it belongs
     * to, so anything above that window covers it. Which window is at \a pos is
     * left to KWin's own hit test, so input shapes and decorations are honoured
     * exactly; only the walk stops early, at the bloomed window itself. Used to
     * keep a thumbnail from acting where it cannot be seen: the input goes to
     * the window covering it, as it does over a shield.
     */
    bool isCovered(KWin::Window *window, const QPointF &pos) const;

    bool pointerMotion(KWin::PointerMotionEvent *event) override;
    bool pointerButton(KWin::PointerButtonEvent *event) override;
    bool pointerAxis(KWin::PointerAxisEvent *event) override;
    bool touchDown(KWin::TouchDownEvent *event) override;

private:
    /*!
     * Moves the focus of \a device off the shield under \a pos, if that is
     * where it is.
     *
     * The pointer and the touch screen keep a focus of their own, but both are
     * InputDeviceHandlers and both are pointed at a shield the same way.
     */
    void redirect(KWin::InputDeviceHandler *device, const QPointF &pos);
    /*!
     * Returns the topmost window at \a pos that is neither bloomed nor one of
     * ours, walking the stacking order with KWin's own hit test. The walk stops
     * empty-handed when it reaches \a stopAt.
     */
    KWin::Window *hitWindow(const QPointF &pos, const KWin::Window *stopAt) const;
    /*! Returns the topmost window at \a pos that is neither bloomed nor one of ours. */
    KWin::Window *windowBelow(const QPointF &pos) const;
    /*! Returns the thumbnail whose click target holds \a pos, if there is one. */
    const Thumbnail *thumbnailAt(const QPointF &pos) const;
    /*! Whether a thumbnail holds \a pos and is visible there, so that it can act. */
    bool isThumbnailUsable(const QPointF &pos) const;

    QRegion m_shields;
    QList<Thumbnail> m_thumbnails;
    QSet<KWin::Window *> m_bloomed;
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
     * Everything the effect keeps around for one bloomed window: where its
     * thumbnail travels from and to, and its click target.
     */
    struct BloomState
    {
        QRectF base; //!< rectangle the layout asked for, before any hover growth
        QRectF from; //!< rectangle the running animation started at
        QRectF to; //!< rectangle the animation ends at
        QRectF current; //!< rectangle used by the current frame
        qreal fromOpacity = 1.0; //!< opacity the running animation started at
        qreal toOpacity = 1.0; //!< opacity the animation ends at
        qreal currentOpacity = 1.0; //!< opacity used by the current frame
        qreal fromCaption = 0.0; //!< caption opacity the running animation started at
        qreal toCaption = 0.0; //!< caption opacity the animation ends at
        qreal currentCaption = 0.0; //!< caption opacity the click target paints with
        qreal fromBend = 0.0; //!< bend strength the running animation started at
        qreal toBend = 0.0; //!< bend strength the animation ends at
        qreal currentBend
            = 0.0; //!< bend strength the current frame is drawn with, 0 flat, 1 full angle
        bool redirected
            = false; //!< whether the window is being painted through an offscreen texture
        bool hovered = false; //!< whether the pointer is on the thumbnail
        QRegion
            hitRegion; //!< part of base left uncovered by system elements, in screen coordinates
        KWin::TimeLine timeline;
        std::unique_ptr<ThumbnailOverlay> overlay;
        std::unique_ptr<OverlayWindow>
            shield; //!< swallows the input the vacated real geometry would still get
    };

    // --- state handling ---

    /*! Rebuilds the layout of every screen and retargets the animations. */
    void relayout();
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
    /*! Applies the thumbnail transformation of \a state to \a data. */
    void applyTransform(
        KWin::EffectWindow *w, const BloomState &state, KWin::WindowPaintData &data) const;
    /*! Whether \a w is one of the thumbnails drawn above the windows covering them. */
    bool isLifted(KWin::EffectWindow *w) const;
    /*! Draws the lifted thumbnails, least enlarged first, if they are still due in this pass. */
    void drawLifted(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport);
    /*!
     * Draws the hover outline just inside the thumbnail of \a w, in logical
     * screen coordinates.
     *
     * The whole \a state is taken rather than a rectangle, because the outline
     * is turned by the same bend as the pixels of the thumbnail and needs the
     * strength this frame is drawn with.
     */
    void drawOutline(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
        KWin::EffectWindow *w, const BloomState &state) const;
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

    // --- window classification ---

    /*! Whether \a w takes part in the layout at all (as thumbnail or as obstacle). */
    bool isRelevant(KWin::EffectWindow *w) const;
    /*! Whether the settings exempt \a w from the effect, so that it neither blooms nor makes others bloom. */
    bool isIgnored(KWin::EffectWindow *w, const QSet<KWin::EffectWindow *> &parents) const;
    /*! Whether \a w may be turned into a thumbnail, honouring the settings. */
    bool isEligible(KWin::EffectWindow *w, const QSet<KWin::EffectWindow *> &parents) const;
    /*! Whether \a w covers its whole maximize area (fullscreen counts as maximized). */
    bool isMaximized(KWin::EffectWindow *w) const;
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
    QTimer m_relayoutTimer;
    std::chrono::milliseconds m_animationDuration { 250 };
    bool m_showIcons = true;
    bool m_showTitles = true;
    qreal m_thumbnailOpacity = 0.9; //!< opacity of a thumbnail that is not hovered
    qreal m_bendAngle
        = 15.0; //!< angle a resting thumbnail is turned by, in degrees; 0 keeps them flat
    LayoutOptions m_layoutOptions;
    QRegion m_systemRegion; //!< screen area covered by panels, popups and other system elements
    ShieldFilter m_shieldFilter;
    TouchDragFilter m_touchDragFilter;
    KWin::Region m_paintRegion; //!< device region the current pass repaints
    KWin::EffectWindow *m_menuOwner
        = nullptr; //!< window whose menu is open, kept focused meanwhile
    KWin::EffectWindow *m_menuPopup = nullptr; //!< the menu itself, watched for its closing
    std::vector<KWin::EffectWindow *>
        m_lifted; //!< thumbnails drawn above the other windows, least enlarged first
    KWin::EffectWindow *m_liftAnchor
        = nullptr; //!< window they are drawn right after, possibly one of them
    bool m_liftPending = false; //!< whether they still have to be drawn in this pass
    bool m_skipKeepAbove = true;
    bool m_skipOnAllDesktops = true;
    bool m_skipMaximized = true;
    bool m_skipParents = true;
    bool m_skipChildren = false;
    bool m_animating = false;
};

} // namespace ThumbnailBloom
