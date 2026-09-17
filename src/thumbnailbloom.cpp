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
#include <core/pixelgrid.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <cursor.h>
#include <input.h>
#include <effect/effecthandler.h>
#include <options.h>
#include <pointer_input.h>
#include <wayland/seat.h>
#include <wayland_server.h>
#include <opengl/glutils.h>
#include <scene/windowitem.h>
#include <window.h>
#include <workspace.h>
#include <effect/effectwindow.h>

#include <KColorScheme>

#include <QGuiApplication>
#include <QHash>
#include <QSet>
#include <QTransform>
#include <QVector2D>
#include <QtMath>

#include <algorithm>
#include <array>
#include <span>
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
 * The finest the grid a bent thumbnail is cut into may get, and how far the
 * pixels it carries may stray from where the perspective puts them, in physical
 * pixels.
 *
 * Texture coordinates are interpolated linearly inside a quad, so a quad drawn
 * as a trapezoid is still textured as if it were a rectangle: cutting the window
 * up is what makes the pixels follow the perspective, each cell being small
 * enough for the error inside it to disappear. How small that has to be is
 * worked out per thumbnail by bendSubdivisions(), rather than a grid fine enough
 * for the worst case being cut every time.
 */
constexpr int maxBendSubdivisions = 16;
constexpr qreal bendTolerance = 0.25;

/*!
 * How many cells a side the thumbnail of \a drawn logical pixels has to be cut
 * into to carry a bend of \a angle degrees without the pixels visibly sliding.
 *
 * Inside one cell the renderer walks the texture at a constant rate while the
 * perspective walks it at a changing one, and the gap between the two is the
 * error the grid exists to hide. It is a second-order one: the map along the
 * bend is u/(1 + k u) with k = sin(angle)/2 for the way bendQuad() places the
 * eye, so its curvature is 2k/(1 + k u)^3, and the most a straight line can
 * stray from a curve over a step of 1/n of its length is that curvature times
 * the step squared over eight. Turned round, the error falls as the square of
 * the cell count, and asking for a quarter of a physical pixel of it settles n.
 *
 * Which is worth doing rather than cutting the finest grid every time: the grid
 * is rebuilt, mapped vertex by vertex and streamed to the card on every frame of
 * every thumbnail, and at the angle the effect actually ships with, a sixth of
 * the cells carry the picture just as truly. A thumbnail that is not bent at all
 * needs no grid whatsoever, and the caller skips the cut entirely.
 */
static int bendSubdivisions(qreal drawn, qreal angle, qreal scale)
{
    const qreal k = std::abs(std::sin(qDegreesToRadians(angle))) / 2.0;
    if (k <= 0.0 || drawn <= 0.0) {
        return 1;
    }

    // The curvature is largest at the near edge of the turn, where the
    // denominator is smallest; u runs over half the side either way.
    const qreal denominator = std::pow(std::max(0.25, 1.0 - k / 2.0), 3.0);
    const qreal tolerance = bendTolerance / std::max(1.0, scale);
    const int cells
        = static_cast<int>(std::ceil(std::sqrt(drawn * k / (4.0 * denominator * tolerance))));
    return std::clamp(cells, 1, maxBendSubdivisions);
}

/*! The frame geometry of \a w as a QRectF, the rectangle all the geometry here runs on. */
static QRectF frameRect(const EffectWindow *w) { return QRectF(w->frameGeometry()); }

/*!
 * Returns how many physical pixels of the screen \a w is on one logical pixel
 * covers, which is the grid everything a thumbnail comes to rest on is rounded
 * to.
 */
static qreal deviceScale(const EffectWindow *w)
{
    const LogicalOutput *screen = w->screen();
    return screen && screen->scale() > 0 ? screen->scale() : 1.0;
}

/*!
 * The fragment shader a thumbnail at rest is drawn with.
 *
 * KWin's own shader for a window under the traits paintSnapshot() asks for,
 * with one thing changed: where it takes a single sample of the texture, this
 * averages the texture over exactly the piece of it the pixel being drawn
 * covers. Everything after the sampling is the stock pipeline, included
 * straight out of KWin's own shader sources, so a thumbnail is coloured,
 * saturated and faded exactly as the window itself would have been.
 *
 * One sample is what makes a shrunk window shimmer. A thumbnail at a third of
 * its window's size has each of its pixels standing for some nine of the
 * window's, and a single sample picks one of the nine and throws the rest away:
 * which one it picks moves as the thumbnail moves, so busy areas crawl, and
 * letters come out thick in one place and thin in the next. Averaging the nine
 * gives the pixel the colour it actually ought to be, and gives it that colour
 * wherever the thumbnail happens to have got to, so a stem of a letter weighs
 * the same at every step of an animation.
 *
 * The mip chain is what makes that affordable. Trilinear filtering already
 * measures the pixel: it takes the very same derivatives and picks the level
 * whose texels are about its size. What it does at that level is the trouble,
 * since one bilinear sample is a tent across two texels wherever it happens to
 * fall rather than the pixel's own footprint, and no level is ever exactly the
 * right size, so it blends the two nearest and comes out soft. This picks the
 * level itself, finest first, and then weighs the pixel out of it properly. A
 * level is only ever reached for when the picture is too coarse for the taps
 * below to weigh the footprint texel by texel, so a thumbnail at a sixth of
 * its window or larger, which is every size the layout ever settles on and a
 * good deal past it, is measured against the full sized picture and comes out
 * exact. A smaller one
 * borrows a level, where the texels in the middle of its footprint are already
 * the averages it would have worked out and only the two at either end are
 * taken as evenly filled.
 *
 * A thumbnail in motion is drawn by the stock shader instead, which takes a
 * single sample off the same chain. What this works out is a good deal more
 * than a moving picture can show, and the steps of a trip are where it can
 * least be afforded, every one of them being a repaint. See paintSnapshot().
 */
static constexpr char filterFragmentSource[] = R"GLSL(
uniform sampler2D sampler;
uniform vec4 modulation;
in vec2 texcoord0;
out vec4 fragColor;

#include "saturation.glsl"
#include "colormanagement.glsl"

// How many pairs of texels one axis of a footprint may be read as, and how wide
// a footprint that leaves room for: one of n texels falls across n + 1 of them
// at worst, so four pairs weigh six texels exactly. A pixel is answered in four
// samples at the sizes the layout settles on, in nine down to a quarter and in
// sixteen at the very worst, and the level below is what keeps the count there
// however small a thumbnail becomes.
const int maxPairs = 4;
const float maxTexels = float(2 * maxPairs - 2);

/*
 * Where to sample pair k of the footprint that runs from a to b, in texels, and
 * how much of the footprint that sample stands for.
 *
 * The two texels of a pair are read as one bilinear sample placed between them,
 * so that the hardware's own interpolation comes out at exactly the weights the
 * two texels are covered by. A whole footprint is therefore read in half as
 * many samples as it covers texels, and every texel is weighted by how much of
 * the pixel it really falls under.
 */
vec2 footprintTap(float a, float b, int k)
{
    float first = floor(a) + float(2 * k);
    float w0 = clamp(min(b, first + 1.0) - max(a, first), 0.0, 1.0);
    float w1 = clamp(min(b, first + 2.0) - max(a, first + 1.0), 0.0, 1.0);
    float weight = w0 + w1;
    return vec2(first + 0.5 + (weight > 0.0 ? w1 / weight : 0.0), weight);
}

/* The texture averaged over the piece of it this pixel covers. */
vec4 footprintAverage()
{
    // The pixel being drawn, pulled back into the picture. The derivatives are
    // how far one pixel of the screen reaches into it in either direction,
    // which is the whole answer: they carry the scale of the thumbnail, the
    // bend, and whatever fraction of a pixel it has got to, without any of it
    // being worked out here. The box around them is what gets averaged.
    vec2 size = vec2(textureSize(sampler, 0));
    vec2 reach = (abs(dFdx(texcoord0)) + abs(dFdy(texcoord0))) * size;

    // The finest level of the chain whose texels the footprint falls across few
    // enough of to be weighed one by one. Level zero for anything down to a
    // sixth, and one level further down for every halving after that, so the
    // work of a pixel never grows however small the thumbnail is drawn.
    float deepest = floor(log2(max(size.x, size.y)));
    float level = clamp(ceil(log2(max(reach.x, reach.y) / maxTexels)), 0.0, deepest);

    // Everything from here on is in the texels of that level, which are asked
    // for rather than halved out of the size above: a level of a picture whose
    // sides are odd rounds down, and its texels are then not quite twice the
    // ones before them.
    int lod = int(level);
    vec2 texels = vec2(textureSize(sampler, lod));
    vec2 centre = texcoord0 * texels;

    // Never narrower than one texel. A footprint of exactly one texel weighs
    // the two texels it straddles by how far it laps onto each, which is
    // ordinary bilinear interpolation, so a thumbnail drawn at its window's own
    // size or larger comes out of this untouched.
    vec2 extent = clamp(reach * texels / size, vec2(1.0), vec2(maxTexels + 1.0));
    vec2 a = centre - 0.5 * extent;
    vec2 b = centre + 0.5 * extent;

    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int j = 0; j < maxPairs; ++j) {
        vec2 tapY = footprintTap(a.y, b.y, j);
        if (tapY.y <= 0.0) {
            continue;
        }
        for (int i = 0; i < maxPairs; ++i) {
            // Worked out here rather than kept in an array of its own: the
            // loops are short and fixed, so the compiler unrolls them and the
            // repeated halves fall together, while an array indexed by a
            // counter can land in memory instead of in registers.
            vec2 tapX = footprintTap(a.x, b.x, i);
            float weight = tapX.y * tapY.y;
            if (weight <= 0.0) {
                continue;
            }
            // The level is named rather than left to the hardware, which would
            // blend this one with the next and undo the point of choosing it.
            sum += weight * textureLod(sampler, vec2(tapX.x, tapY.x) / texels, level);
            total += weight;
        }
    }
    return sum / total;
}

void main()
{
    vec4 result = footprintAverage();
    result = encodingToNits(result, sourceNamedTransferFunction,
        sourceTransferFunctionParams.x, sourceTransferFunctionParams.y);
    result.rgb = (colorimetryTransform * vec4(result.rgb, 1.0)).rgb;
    result = adjustSaturation(result);
    result *= modulation;
    result.rgb = doTonemapping(result.rgb);
    result = nitsToDestinationEncoding(result);
    fragColor = result;
}
)GLSL";

/*!
 * How many levels the mip chain of a texture \a size pixels large has.
 *
 * One for the picture itself and one for every halving of it down to a single
 * pixel, which is the whole chain. The shader above reaches for a level only
 * once a thumbnail is drawn below a sixth of its window's size, while the
 * trilinear path every thumbnail in motion takes reaches for all of them, so
 * the tail earns its keep either way: every level but the first is a quarter of
 * the one above it, which makes the whole chain a third of the picture again.
 */
static int mipLevels(const QSize &size)
{
    return 1 + static_cast<int>(std::floor(std::log2(std::max(size.width(), size.height()))));
}

/*! Returns \a rect with every edge on the nearest whole physical pixel at \a scale. */
static QRectF roundToDevice(const QRectF &rect, qreal scale)
{
    const auto snap = [scale](qreal value) { return std::round(value * scale) / scale; };
    return QRectF(QPointF(snap(rect.left()), snap(rect.top())),
        QPointF(snap(rect.right()), snap(rect.bottom())));
}

/*!
 * Returns the smallest number of whole logical pixels that is a whole number of
 * physical ones at \a scale.
 *
 * A window may only be put at whole logical pixels, so moving one in steps of
 * this is the only way to keep it on the physical pixel grid: two logical pixels
 * at a scale of 1.5, four at 1.25, one at every whole scale. A scale that does
 * not come out even within eight of them is left at one, the fraction being far
 * too small to be worth a larger buffer.
 */
static int deviceGridStep(qreal scale)
{
    for (int step = 1; step <= 8; ++step) {
        const qreal pixels = step * scale;
        if (std::abs(pixels - std::round(pixels)) < 1e-3) {
            return step;
        }
    }
    return 1;
}

/*! Returns \a value rounded down to a whole multiple of \a step. */
static int floorToStep(qreal value, int step)
{
    return static_cast<int>(std::floor(value / step)) * step;
}

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
 *
 * \a area is what the thumbnail has to stay inside, and the caller hands in the
 * room the frame needs as well: the line is drawn outside the thumbnail, so a
 * grown one pushed flat against the work area would have its frame drawn off the
 * screen or over the panel next to it.
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
 * Returns how much larger than its resting thumbnail \a current is drawn.
 *
 * One while the thumbnail sits where the layout put it, more while the hover has
 * it grown, and more still while it travels back to the real window; it is what
 * orders the lifted thumbnails among themselves. The reference is the resting
 * thumbnail even then, never the window itself, so the trip home reads as a
 * growth rather than as the shrink measuring against the real geometry makes of
 * it.
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

/*!
 * Returns whether the thumbnail \a rect of the window \a id would be drawn
 * under a backdrop: one of \a stack that is stacked above it and overlaps it.
 *
 * A thumbnail is painted at the stacking position of its own window, so a
 * backdrop above that window would simply cover it; those are the thumbnails
 * the paint pass has to lift out of the stacking order. \a placed are the
 * windows that got a thumbnail of their own, which a backdrop can be when the
 * settings let a maximized window bloom: it is then leaving its own geometry and
 * covers nothing to be lifted over.
 */
static bool underBackdrop(const QList<LayoutWindow> &stack, const void *id, const QRectF &rect,
    const QSet<EffectWindow *> &placed)
{
    // From the top down, stopping at the window itself: only what is above it
    // can be painted over its thumbnail.
    for (int i = stack.size() - 1; i >= 0; --i) {
        if (stack[i].id == id) {
            return false;
        }
        if (stack[i].backdrop && stack[i].geometry.intersects(rect)
            && !placed.contains(static_cast<EffectWindow *>(stack[i].id))) {
            return true;
        }
    }
    return false;
}

//! Width of the outline of a thumbnail the pointer is on, in logical pixels.
constexpr qreal hoverOutlineWidth = 6.0;

//! Width of the outline of a thumbnail at rest, in logical pixels.
constexpr qreal restOutlineWidth = 1.0;

/*!
 * How far past its own rectangle a thumbnail is drawn, in logical pixels.
 *
 * The frame is drawn outside that rectangle and put on the physical pixel grid,
 * which can leave its inner edge a fraction of a pixel clear of the picture, and
 * two antialiased edges meeting would leave a row of half-covered pixels between
 * them in any case. So the thumbnail is bled outwards instead: the frame is the
 * thing that has to be sharp, and a picture stretched by half a pixel is a
 * picture nobody can tell from the one it was. The line is drawn over the bleed
 * and hides it.
 */
constexpr qreal thumbnailBleed = 0.5;

//! Strength below which there is no outline left to paint.
constexpr qreal outlineEpsilon = 1e-2;

/*!
 * How far the frame in the store may be off the thumbnail before the draw puts
 * it right, in logical pixels.
 *
 * The same twentieth of a pixel ThumbnailCanvas::setContent() calls a change
 * worth repainting, and it has to be: a difference it drops is one no transform
 * should be spent on either, since the store then holds the frame this one asked
 * for and correcting it would resample the line for nothing.
 */
constexpr qreal outlineSlack = 0.05;

/*!
 * Margin kept around a thumbnail wherever its ground is measured, in logical
 * pixels: the widest the outline is ever drawn, a pixel for the coverage ramp
 * that antialiases it, and one more for the snap that puts the line on the
 * physical pixel grid.
 */
constexpr qreal paintMargin = hoverOutlineWidth + 2.0;

//! How far past its resting growth of 1 a thumbnail must be drawn to count as lifted.
constexpr qreal liftEpsilon = 1e-3;

/*!
 * Returns \a from mixed with \a to at \a t, straight down the components,
 * alpha included.
 */
static QColor mixColors(const QColor &from, const QColor &to, qreal t)
{
    const auto mix = [t](qreal a, qreal b) { return a + (b - a) * t; };
    return QColor::fromRgbF(mix(from.redF(), to.redF()), mix(from.greenF(), to.greenF()),
        mix(from.blueF(), to.blueF()), mix(from.alphaF(), to.alphaF()));
}

/*! Returns the colour a thumbnail under the pointer is outlined in. */
static QColor hoverOutlineColor()
{
    return KColorScheme(QPalette::Active, KColorScheme::View)
        .decoration(KColorScheme::FocusColor)
        .color();
}

/*! Returns the colour a thumbnail at rest is outlined in. */
static QColor restOutlineColor()
{
    // The colour the captions are drawn in, so that the outline and the title of
    // a thumbnail read as the one frame around it.
    return KColorScheme(QPalette::Active, KColorScheme::Window).foreground().color();
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
 * Returns whether \a w is a popup that owns the input while it is up without
 * ever saying so through a grab.
 *
 * hasPopupGrab() is answered by the shell surfaces alone. An X11 menu is an
 * override redirect window that takes a pointer grab of its own, directly on the
 * server, and KWin is never told: the window type is all there is to go on
 * there, so the three kinds that are always opened with a grab are read as one.
 * The types are listed rather than isPopupWindow() asked because that counts
 * tooltips in, and a tooltip grabs nothing.
 */
static bool grabsInputByType(EffectWindow *w)
{
    return w->isX11Client() && (w->isPopupMenu() || w->isDropdownMenu() || w->isComboBox());
}

/*!
 * Adds every window \a w is transient for, however deep, to \a owners.
 *
 * \a w itself is left out: the question is whose input something has taken
 * over, and the window holding the grab is never a thumbnail anyway.
 */
static void addOwners(EffectWindow *w, QSet<EffectWindow *> &owners)
{
    const QList<EffectWindow *> mainWindows = w->mainWindows();
    for (EffectWindow *parent : mainWindows) {
        // The set doubles as the visited mark, so a cycle in the transient chain
        // (nothing stops a client from building one) ends the walk here instead
        // of recursing for ever.
        if (!owners.contains(parent)) {
            owners.insert(parent);
            addOwners(parent, owners);
        }
    }
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

/*! Whether a mouse button is down right now. */
static bool pointerButtonHeld() { return input()->pointer()->buttons() != Qt::NoButton; }

/*! Whether a drag and drop is being carried across the screen right now. */
static bool dragInProgress()
{
    WaylandServer *server = waylandServer();
    return server && server->seat() && server->seat()->isDrag();
}

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
    : m_dragDropFilter(m_shieldFilter)
{
    input()->installInputEventFilter(&m_shieldFilter);
    input()->installInputEventFilter(&m_touchDragFilter);
    input()->installInputEventFilter(&m_dragDropFilter);

    // A drag that rests long enough on a thumbnail asks for the window itself,
    // which is the same thing a click on it asks for.
    m_dragDropFilter.setActivationHandler([](Window *window) {
        if (EffectWindow *w = window->effectWindow()) {
            effects->activateWindow(w);
        }
    });

    // A click that went into the window widens the pointer's hold on the
    // thumbnail to the whole of the enlarged picture, since that is what
    // whatever the click started is being aimed at.
    m_shieldFilter.setClickHandler([this](Window *window) {
        EffectWindow *w = window->effectWindow();
        const auto it = w ? m_states.find(w) : m_states.end();
        if (it != m_states.end() && !it->second.clicked) {
            it->second.clicked = true;
            scheduleRelayout();
        }
    });

    // Input put into the window through its thumbnail is the user aiming at that
    // thumbnail, which is what a thumbnail bloomed under a resting cursor is
    // waiting for before it may grow.
    m_shieldFilter.setEngageHandler([this](Window *window) { engage(window); });

    // A second finger on a thumbnail means the gesture is for the window, so the
    // click target has to let go of the one it was following.
    m_shieldFilter.setTouchTakenOverHandler([this](Window *) {
        // At most one click target is following a finger at any moment, so
        // every one of them is told; the rest have nothing to let go of.
        for (auto &[screen, input] : m_input) {
            if (input.target) {
                input.target->cancelTouch();
            }
        }
    });

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
        // The effect's own windows say nothing about the layout, and the frame
        // store is moved by every step of every animation: watching that would
        // run a layout pass a frame.
        if (isOwnOverlay(w)) {
            return;
        }
        watch(w);
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::windowClosed, this, [this](EffectWindow *w) {
        if (w == m_menuPopup) {
            m_menuPopup = nullptr;
            m_menuOwner = nullptr;
        }
        // One of the effect's own going away says nothing about the layout,
        // and it only ever goes away from the pass that has just run.
        if (isOwnOverlay(w)) {
            return;
        }
        forget(w);
        scheduleRelayout();
    });
    connect(effects, &EffectsHandler::windowDeleted, this, [this](EffectWindow *w) { forget(w); });
    connect(effects, &EffectsHandler::windowActivated, this,
        [this](EffectWindow *) { scheduleRelayout(); });
    // Not while the effect is putting up or taking down a window of its own:
    // that restacks nothing the layout reads, and a pass over again for every
    // click target shown would double the cost of every bloom.
    connect(effects, &EffectsHandler::stackingOrderChanged, this, [this]() {
        if (!m_ownWindowChange) {
            scheduleRelayout();
        }
    });
    // Fires both ways, so the same pass that stands the effect down brings it
    // back once the full screen effect is over; see standDown().
    connect(effects, &EffectsHandler::hasActiveFullScreenEffectChanged, this,
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
    // The hover is frozen while a button is held, so the last button coming back
    // up is what picks it up again. The signal comes from KWin's own button
    // bookkeeping rather than from a filter, so it arrives whoever ends up
    // consuming the event (a move, a popup) and the state it reads is already
    // the new one.
    connect(input(), &InputRedirection::pointerButtonStateChanged, this,
        [this](uint32_t, PointerButtonState) {
            if (!pointerButtonHeld()) {
                updateHover(effects->cursorPos());
            }
        });

    // Wherever the pointer already is counts as where it was, so the first
    // thumbnail to bloom under a cursor that has not moved yet is sat on rather
    // than arrived at, like any other.
    m_hoverPos = effects->cursorPos();

    for (EffectWindow *w : effects->stackingOrder()) {
        watch(w);
    }

    // The colours the frame is drawn between are kept rather than read per
    // frame, and this is the only announcement Qt 6 makes of a colour scheme
    // change.
    qApp->installEventFilter(this);

    reconfigure(ReconfigureAll);
}

bool ThumbnailBloomEffect::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        m_outlineDirty = true;
        // The captions hold the colours of the scheme as well, and this one
        // filter tells every store rather than each of them watching the
        // application on its own.
        for (auto &[w, state] : m_states) {
            if (state.canvas) {
                state.canvas->invalidateCaption();
            }
        }
        // Nothing else moves, so the frames of the thumbnails standing still
        // would keep the old colour until something did.
        effects->addRepaintFull();
    }

    return Effect::eventFilter(watched, event);
}

ThumbnailBloomEffect::~ThumbnailBloomEffect()
{
    // Destroying an input window makes KWin drop its internal window and emit
    // windowClosed synchronously, and that handler walks m_states: every
    // handler touching the map must be gone before the maps are destructed, or
    // forget() re-enters a container that is going away.
    disconnect(effects, nullptr, this, nullptr);
    disconnect(Cursors::self(), nullptr, this, nullptr);
    for (auto &entry : m_states) {
        disconnect(entry.first, nullptr, this, nullptr);
    }

    // The states hold the offscreen stores, and freeing a texture is a call into
    // the driver like any other: it needs the context that made it, and nothing
    // makes that context current for an effect being unloaded.
    if (effects->isOpenGLCompositing() && !EglContext::currentContext()) {
        effects->makeOpenGLContextCurrent();
    }
    m_filterShader.reset();
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

    m_dragDropFilter.setActivationDelay(ThumbnailBloomConfig::dragActivationDelay());

    // The store every window is drawn through has nothing to do with the angle:
    // turning it down to zero leaves the thumbnails flat, drawn out of the very
    // same texture.
    m_bendAngle = std::clamp<qreal>(ThumbnailBloomConfig::bendAngle(), 0.0, 60.0);

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
            // What the store holds is a frame of the window, and this is the
            // only word there is that it has moved on. Nothing is drawn here:
            // the store is brought up to date by the next paint that draws from
            // it, so a window changing faster than the screen refreshes costs
            // one redraw per frame rather than one per change.
            it->second.stale = true;
            effects->addRepaint(RectF(paintedArea(window, it->second)));
        }
    });
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ThumbnailBloomEffect::scheduleRelayout() { m_relayoutTimer.start(); }

void ThumbnailBloomEffect::relayout()
{
    // Overview, Window View, Desktop Grid and the desktop slide all put the
    // session up as it really is, so nothing of the effect's may be on the screen
    // while one of them runs. There is no meeting them half way: they fly each
    // window from the rectangle it really occupies (WindowHeapDelegate reads
    // Window::frameGeometry through its QML properties), which a thumbnail never
    // changes, and no effect can tell another where it is drawing. So the bloom
    // gets out of the way instead.
    if (effects->hasActiveFullScreenEffect()) {
        standDown();
        return;
    }

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
        updateInputWindows();
        updateHover(effects->cursorPos());
        return;
    }

    // Which windows take part at all is asked once and kept, in stacking order:
    // the transient parents have to be complete before the first window can be
    // judged, and walking the whole stack twice over to get that would ask it of
    // every window twice.
    std::vector<EffectWindow *> relevant;
    m_relevantWindows.clear();
    for (EffectWindow *w : effects->stackingOrder()) {
        if (isRelevant(w)) {
            relevant.push_back(w);
            // Kept for the paint pass, which needs the same answer for every
            // window of the session on every frame and must not work it out
            // fifteen calls at a time; see m_relevantWindows.
            m_relevantWindows.insert(w);
        }
    }

    const QSet<EffectWindow *> parents = transientParents(relevant);

    // Worked out once and handed to everything below: being ignored is the
    // larger half of being ineligible, it decides which window speaks for its
    // screen, and it is the half that can cost a screen lookup.
    // Whether a window fills its screen is asked three times over by what
    // follows, and each asking is a work area lookup, so it is asked once here.
    std::vector<bool> maximized;
    std::vector<bool> ignored;
    maximized.reserve(relevant.size());
    ignored.reserve(relevant.size());
    for (EffectWindow *w : relevant) {
        maximized.push_back(isMaximized(w));
        ignored.push_back(isIgnored(w, maximized.back(), parents));
    }

    updateBackdropScreens(relevant, ignored, maximized);

    // Windows only ever collide with windows of their own screen, so each screen
    // is laid out on its own. The stacking order is preserved per screen.
    EffectWindow *active = effects->activeWindow();
    QHash<LogicalOutput *, QList<LayoutWindow>> perScreen;
    for (size_t i = 0; i < relevant.size(); ++i) {
        EffectWindow *w = relevant[i];
        perScreen[w->screen()].append(LayoutWindow { w, frameRect(w), isEligible(w, ignored[i]),
            w == active, ignored[i], maximized[i] && m_backdropScreens.contains(w->screen()) });
    }

    // Asked here and handed on rather than folded into the layout above: a grab
    // is no layout input at all, and the placements have to come out exactly as
    // they would have without it; see applyPlacements().
    applyPlacements(perScreen, blockedWindows());
}

QSet<EffectWindow *> ThumbnailBloomEffect::blockedWindows() const
{
    QSet<EffectWindow *> blocked;
    for (EffectWindow *w : effects->stackingOrder()) {
        // KWin's own surfaces are passed over, the window menu of a thumbnail
        // among them: it grabs like any other popup, but it is opened on the
        // thumbnail and belongs to it, so the thumbnail has to stay.
        if (w->isDeleted() || !w->isVisible() || w->internalWindow()) {
            continue;
        }
        if (grabsInput(w) || grabsInputByType(w)) {
            addOwners(w, blocked);
        }
    }
    return blocked;
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
    const QHash<LogicalOutput *, QList<LayoutWindow>> &perScreen,
    const QSet<EffectWindow *> &blocked)
{
    QSet<EffectWindow *> bloomed;
    // Placed windows a grab of their own is holding out of their bloom; see showInPlace().
    QSet<EffectWindow *> held;
    for (auto it = perScreen.cbegin(); it != perScreen.cend(); ++it) {
        const QRectF workArea = effects->clientArea(MaximizeArea, it.key());

        // A screen that has just taken the backdrop exception back shows every
        // thumbnail it has at once, so they all set off from the one point
        // instead of from the windows they belong to. Nothing else changes: they
        // are laid over the backdrop as always, which draws them under the
        // window that speaks for the screen and so has them come out from behind
        // it.
        const bool bursting = m_burstScreens.contains(it.key());
        const QPointF burst = bursting ? burstPoint(it.key()) : QPointF();

        for (const Placement &placement : computeLayout(it.value(), workArea, m_layoutOptions)) {
            EffectWindow *w = static_cast<EffectWindow *>(placement.id);

            // Something the window put up has taken the input over: a menu, a
            // drop down, anything opened with a grab. It is drawn against the
            // real surface, which is nowhere near the thumbnail, and the grab
            // leaves the click target inert, so the window is shown where it
            // really is until the grab is over. Its placement is computed and
            // thrown away rather than never asked for: the layout has to come
            // out exactly as it would have, so that the thumbnails around it
            // hold still and it blooms back into this very rectangle afterwards.
            // It stays out of `bloomed`, which speaks for the thumbnails alone
            // (a blocked window really is where it is, backdrop or not).
            if (blocked.contains(w)) {
                held.insert(w);
                showInPlace(w);
                continue;
            }

            bloomed.insert(w);
            retarget(w, placement.rect, bursting ? &burst : nullptr);
            // The grab is over: an ordinary thumbnail again, drawn out of the
            // stacking order only for the reasons every other one is.
            m_states.at(w).blocked = false;
            // The paint pass draws such a thumbnail out of the stacking order,
            // over the backdrop hiding it; asked here rather than per frame,
            // since it can only change with the layout that decided it.
            // The placements arrive top down, so a backdrop above this window has
            // already been entered into `bloomed` if it is leaving itself.
            m_states.at(w).overBackdrop = underBackdrop(it.value(), w, placement.rect, bloomed);
        }
    }

    // Windows that stopped blooming travel back to where they really are.
    EffectWindow *const active = effects->activeWindow();
    for (auto &[w, state] : m_states) {
        // The held ones are already home and stay there. Only the ones the
        // layout placed: a window with no placement of its own to come back to
        // (the active one, whose menu is up as often as not) is an ordinary
        // window that has stopped blooming, grab or no grab.
        if (bloomed.contains(w) || held.contains(w)) {
            continue;
        }
        state.hovered = false;
        state.clicked = false;
        state.blocked = false;
        // Clicking a thumbnail raises its window over the backdrop, and a
        // backdrop that stopped being one covers nothing either way, so a
        // window travelling home no longer needs to be drawn out of turn. The
        // dive is drawn out of turn on its own account; see retarget().
        state.overBackdrop = false;

        // A screen that has just lost the backdrop exception loses every
        // thumbnail on it at once, and travelling home would show none of that:
        // the window is under the one that took the screen over, which is why it
        // had a thumbnail in the first place. They shrink into the point instead.
        //
        // The window that was just picked is the exception. It is on its way to
        // being looked at, so it goes home as always, whatever its screen is
        // doing. A thumbnail already diving keeps at it, since the burst of
        // relayouts a single raise produces would otherwise call the dive off
        // one pass after it began.
        const bool diving = w != active && !sameRect(state.base, frameRect(w))
            && (state.diving || m_diveScreens.contains(w->screen()));
        retarget(w, diving ? QRectF(burstPoint(w->screen()), QSizeF(0, 0)) : frameRect(w));
    }

    // Needs the final hit regions of every thumbnail, so it comes after the
    // whole layout rather than per window.
    updateInputWindows();

    // The click targets have just been placed and moved, so the pointer can end
    // up on another thumbnail without having moved at all.
    updateHover(effects->cursorPos());
}

void ThumbnailBloomEffect::showInPlace(EffectWindow *w)
{
    // Nothing may be aimed at the thumbnail any more, and the hover has to go
    // before the retarget rather than after it: the retarget reads it to work out
    // where the window is heading, and one sent home with the hover still
    // standing would set off towards the grown rectangle of a thumbnail that is
    // on its way out.
    const auto it = m_states.find(w);
    if (it != m_states.end()) {
        it->second.hovered = false;
        it->second.clicked = false;
        // Whatever the thumbnail was drawn over, the window itself is under it
        // again; the lift it needs now is the blocked one, worked out per frame
        // by updateLift().
        it->second.overBackdrop = false;
    }

    // Home is the window's own geometry, which is what takes the click target
    // and the shield down with it: both read the destination and let go of a
    // window that is no longer a thumbnail, handing it its own input back, grab
    // and all.
    retarget(w, frameRect(w));

    // Marked after the retarget, which is what creates the state if the window
    // had none: one whose menu was already open when something else covered it
    // has never bloomed at all, and it has to be drawn out of the stacking order
    // just the same.
    BloomState &state = m_states.at(w);
    state.blocked = true;

    // Every retarget hands the window back a store, and a held window is
    // retargeted by every relayout that comes along. Once the trip home is over
    // the window is drawn where it really is and at its own size, so there is
    // nothing left for a store to do: the release advanceAnimations() made on
    // arrival is held rather than undone frame after frame.
    if (state.timeline.done()) {
        setSnapshot(w, state, false);
    }
}

void ThumbnailBloomEffect::retarget(
    EffectWindow *w, const QRectF &placement, const QPointF *burst)
{
    const auto [it, inserted] = m_states.try_emplace(w);
    BloomState &state = it->second;

    // A window travelling back to its real geometry is on its way to being an
    // ordinary window again, so it fades back to fully opaque just like the
    // hovered thumbnail does.
    const bool thumbnail = !sameRect(placement, frameRect(w));

    // Rounded here and only here: what a thumbnail comes to rest on is what it
    // is drawn at for as long as nothing moves, so an edge of it half a physical
    // pixel off is resampled the whole time, while a step of an animation is
    // gone before it can be looked at and rounding those would cost the motion
    // its evenness. The trip home is left alone, its destination being where the
    // window really is.
    const qreal scale = deviceScale(w);
    const QRectF base = thumbnail ? roundToDevice(placement, scale) : placement;
    state.base = base;

    // A destination with no size at all is the dive: the thumbnail is heading
    // for the point its whole screen collapses into rather than for a place it
    // could be seen, and it takes everything that makes it a thumbnail down with
    // it. Asked of a real destination only, so that a window with no geometry to
    // speak of is still sent home rather than made to dive into itself.
    const bool diving = thumbnail && base.isEmpty();
    const bool wasDiving = state.diving;
    state.diving = diving;

    // The burst only puts a thumbnail at the point when it is not already on its
    // way there: a dive turned around halfway carries on from wherever it has
    // got to, exactly as every other reversed trip does, rather than jumping
    // back to the point first.
    const bool bursting = burst && !wasDiving;
    // Whether the thumbnail is drawn above its resting size at this very moment,
    // measured against the rectangle it was resting at before this trip: it has
    // to be read before that rectangle is replaced below.
    const bool liftedNow = growth(state.rect.current, state.thumbBase) > 1.0 + liftEpsilon;
    // The rectangle the lift measures against. It only ever follows a real
    // thumbnail, so the window travelling home keeps the one it is leaving:
    // measuring the trip against the real geometry would call it a shrink and
    // drop the window behind the thumbnails it is growing past.
    // A dive is measured against the thumbnail it is leaving, exactly as the trip
    // home is: its own destination has no size at all and would say nothing.
    if ((thumbnail && !diving) || inserted) {
        state.thumbBase = base;
    }
    // The work area less the room the frame takes outside the thumbnail. A
    // thumbnail at rest needs no such thing, the layout keeping its own margin
    // from the edge of the screen, and that margin is the wider of the two.
    const QRectF target = state.hovered && !diving
        ? roundToDevice(grownRect(base, frameRect(w),
                            QRectF(effects->clientArea(MaximizeArea, w))
                                .adjusted(paintMargin, paintMargin, -paintMargin, -paintMargin)),
              scale)
        : base;
    // What the thumbnail is actually drawn at once it gets there, which is what
    // a point on it has to be measured against: the picture the pointer is
    // aiming at is the grown one, not the rectangle the layout handed out.
    state.hoverRect = target;
    // A thumbnail diving into the point fades out as it goes, so the last frames
    // of it, where there is barely a thumbnail left to scale, are not seen at all.
    const qreal targetOpacity
        = diving ? 0.0 : ((thumbnail && !state.hovered) ? m_thumbnailOpacity : 1.0);
    // The caption belongs to the resting thumbnail only: it fades out under the
    // pointer, and on the way back to the real window (or into the point) it is
    // gone before the trip is over.
    const qreal targetCaption = (thumbnail && !diving && !state.hovered) ? 1.0 : 0.0;
    // The bend belongs to the resting thumbnail just as the caption does: it
    // flattens out under the pointer, so a hovered window is seen head on, and it
    // is gone before the window is back where it really is.
    const qreal targetBend = targetCaption;
    // Not the outline itself, which every thumbnail has, but how heavily it is
    // drawn: the pointer alone thickens it and turns it to the focus colour, and
    // every other trip a thumbnail makes (blooming out, being relaid out,
    // travelling home) leaves it at the thin caption-coloured line.
    const qreal targetHighlight = state.hovered ? 1.0 : 0.0;

    // Which trip this is, which is what the lift follows: the size the thumbnail
    // happens to be drawn at along the way decides nothing, since judging by that
    // would lift every thumbnail heading for a smaller rectangle (it is above
    // that one until it arrives) and draw it over everything, the active window
    // included, for the length of every layout change.
    const bool goingHome = !thumbnail;

    // Whether this is the same trip home the thumbnail was already on, which is
    // what lets it keep its clock below rather than starting over.
    const bool following = goingHome && state.homing && !inserted;
    state.homing = goingHome;

    if (diving) {
        // Everything a dive shrinks into sits under the window that has just
        // taken the screen over, so the whole set is drawn out of the stacking
        // order for as long as the dive lasts. Its own depth would show nothing
        // of it at all, and the shrink alone is not enough for the size test
        // below to lift it.
        state.lift = Lift::Dive;
    } else if (state.hovered) {
        // The pointer's own growth.
        state.lift = Lift::Hover;
    } else if (goingHome) {
        // Only the picked thumbnail is drawn over the rest on the way back to its
        // window. Any other window that stops blooming (one that came uncovered,
        // one the settings just exempted) travels home at the depth its stacking
        // position gives it, like every other thumbnail in motion.
        state.lift = w == effects->activeWindow() ? Lift::Home : Lift::None;
    } else if (state.lift == Lift::Hover && liftedNow) {
        // The growth is undone at the pointer's pace: the hover is over the
        // moment the pointer leaves, but the thumbnail is still drawn above its
        // resting size and keeps its lift until it has come all the way down.
        // This is the only trip that carries a lift into the next one. A
        // thumbnail that is merely drawn large for some other reason (one caught
        // half way home by a relayout that blooms it again) is starting an
        // ordinary trip, and lifting it for the size it happens to have would put
        // it over the active window for as long as that trip lasts.
        state.lift = Lift::Hover;
    } else {
        state.lift = Lift::None;
    }

    // Every thumbnail is drawn through a store of its own, bend or no bend: one
    // is drawn smaller than the window it shows either way, which is what the
    // mip chain in the store is there for. It stays up for as long as the window
    // blooms.
    setSnapshot(w, state, true);

    // The click target follows the resting rectangle, not the animation: a
    // thumbnail can be hovered and clicked from the moment it sets off, but only
    // where it is going to end up. The store is put up here as well, for a reason
    // of its own: both are windows, and a window may only be shown or hidden from
    // a relayout.
    updateHitRegion(w, state);
    updateCanvas(w, state);

    if (inserted) {
        state.rect.snap(frameRect(w));
        state.opacity.snap(1.0);
        state.caption.snap(0.0);
        state.bend.snap(0.0);
        state.highlight.snap(0.0);
    } else if (!bursting && sameRect(state.rect.to, target)
        && qFuzzyCompare(state.opacity.to, targetOpacity)
        && qFuzzyCompare(state.caption.to, targetCaption)
        && qFuzzyCompare(state.bend.to, targetBend)
        && qFuzzyCompare(state.highlight.to, targetHighlight)) {
        return;
    }

    // The burst puts every thumbnail of its screen at the one point before any
    // of them sets off, so that they all appear to come out of the window that
    // speaks for the screen. It is applied after the insertion above, which
    // would otherwise start a new thumbnail at the window it belongs to.
    if (bursting) {
        state.rect.snap(QRectF(*burst, QSizeF(0, 0)));
        state.opacity.snap(0.0);
        state.caption.snap(0.0);
        state.bend.snap(0.0);
        state.highlight.snap(0.0);
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
    // curve: a thumbnail that is already in motion switches to a curve that
    // starts at full speed and only eases out, which picks up where the previous
    // trip left off closely enough for the eye.
    state.rect.restart(target);
    state.opacity.restart(targetOpacity);
    state.caption.restart(targetCaption);
    state.bend.restart(targetBend);
    state.highlight.restart(targetHighlight);

    // The trip home is the one exception: it is re-aimed, never started over. Its
    // destination is the window's own geometry, which the user can be dragging
    // around while the thumbnail chases it, and the geometry changes arrive per
    // pointer event rather than per frame. Restarting there resets the clock
    // oftener than the frames advance it, so the thumbnail crawls behind the
    // window and the trip never ends, which is also what keeps the state alive:
    // it is dropped on the frame the trip is over. Re-aiming from wherever the
    // thumbnail has got to and letting the clock run ends it on time whatever the
    // window did in the meantime, and the window is drawn where it really is from
    // then on. Once the clock is spent, every further re-aim lands the thumbnail
    // on the window at once, so it follows exactly until it is dropped.
    if (!following) {
        state.timeline.setEasingCurve((inserted || bursting || state.timeline.done())
                ? QEasingCurve::InOutCubic
                : QEasingCurve::OutCubic);
        state.timeline.setDuration(m_animationDuration);
        state.timeline.reset();
    }

    // The ground this trip sets off from: where the thumbnail was last painted,
    // where it stands at this moment (which is the real window, shadow and all,
    // when a window is only just blooming out) and the rectangle it is heading
    // for. Everything past the first frame is asked for by the paint pass itself,
    // which widens the damage of the frame by the ground every running animation
    // moves over and then asks for the next frame over the same, so this is the
    // one repaint a whole trip needs.
    const QRectF ground
        = thumbnailBounds(w, state.rect.current).united(state.painted).united(state.base);
    if (!ground.isNull()) {
        effects->addRepaint(
            RectF(ground.adjusted(-paintMargin, -paintMargin, paintMargin, paintMargin)));
    }
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
    // Everything with a hit region takes part, animating or not: the region
    // sits on the destination of the thumbnail, so the hit test never follows
    // it along its path.
    const auto targetable = [](const BloomState &state) { return !state.hitRegion.isEmpty(); };

    // This runs on every step the pointer takes, for the whole life of the
    // session, and with nothing bloomed there is no thumbnail for one to arrive
    // at. The position is still kept: a thumbnail blooming under a cursor that
    // has not moved has to read as one sat on rather than one arrived at, and
    // that is the comparison which says so.
    if (m_states.empty()) {
        m_hoverPos = pos;
        return;
    }

    // Reduced motion: a thumbnail growing under the pointer while a window is
    // being dragged is motion nobody asked for, since the pointer is only
    // passing over it on its way somewhere else. Nothing is hovered until the
    // drag ends, and the pass the finish signal schedules picks the hover back
    // up from wherever the pointer came to rest. Where the pointer went
    // meanwhile is still followed, so that the thumbnails the dropped window
    // rearranges are ones the cursor is sitting on rather than ones it has just
    // arrived at, exactly as if they had bloomed under it.
    if (layoutFrozen()) {
        for (const auto &[w, state] : m_states) {
            setHovered(w, false);
        }
        m_hoverPos = pos;
        return;
    }

    // The menu of a thumbnail belongs to that thumbnail, and opening it is
    // aiming at it exactly as a scroll is, so it takes the hover whether or not
    // the pointer had arrived on it first. It is a popup, so it takes the
    // pointer and is cut out of the hit region, and the thumbnail would shrink
    // away under its own menu; it stays hovered until the menu is gone instead. The menu closing is a window closing, which schedules the
    // relayout that ends this.
    if (EffectWindow *owner = menuOwner()) {
        for (const auto &[w, state] : m_states) {
            setHovered(w, w == owner);
        }
        m_hoverPos = pos;
        return;
    }

    // A held button means the pointer is busy with whatever it went down on, so
    // the hover is left exactly as the press found it: one pressed on a
    // thumbnail keeps that thumbnail, and one pressed anywhere else (a selection
    // being pulled out, a window being resized) grows no thumbnail on its way
    // over. The release signal runs this again from wherever the pointer came to
    // rest. A drag and drop is the exception: it is carried with the button down
    // and it is going somewhere, so a thumbnail it comes to rest on lights up
    // like any other drop target. The pointer is followed all the same, so the
    // release finds it where it left it and nothing has been arrived at: a
    // thumbnail the button was dragged across, or came down on, is one the
    // cursor is sitting on, and it grows when the cursor is next moved to it.
    if (pointerButtonHeld() && !dragInProgress()) {
        m_hoverPos = pos;
        return;
    }

    // A hover has to be arrived at. The pointer being on a thumbnail is not the
    // same as its having come to one: a window raised under a resting cursor
    // puts a thumbnail there without the pointer moving at all, and a picture
    // that jumps out from under the cursor is the one thing nobody asked for. So
    // a hover only ever starts on a step that crosses into the thumbnail, which
    // is what the step before says. That step is put the very same question this
    // one is, rather than merely whether it lay in the hit region: the region is
    // only half of what makes a thumbnail hoverable, the other half being
    // whether a window covers it at that spot (the shadow of a client, which
    // takes input for the resize border drawn inside it), and a pointer crossing
    // out of the covered part onto the picture has arrived at it just as much as
    // one crossing the edge of the region. Putting one question to both points
    // is also what leaves a pointer that has not moved unable to start anything,
    // whatever has gone on around it meanwhile; one that left the thumbnail and
    // came back has the step before outside it and grows it as ever.
    // A hover already running is never asked at all: it was arrived at once and
    // keeps the pointer for as long as it stays on.
    // A drag is the exception, as it is to the held button above: it is going
    // somewhere, so it lights up what it rests on however that got there.
    EffectWindow *hovered = thumbnailUnder(pos);
    if (hovered && !dragInProgress() && !m_states.at(hovered).hovered
        && thumbnailUnder(m_hoverPos) == hovered) {
        hovered = nullptr;
    }
    m_hoverPos = pos;

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
    // A window painted over the thumbnail takes that part of it away: the
    // pointer is on the window, not on a thumbnail it cannot see, so no hover
    // starts there. It ends none either. A thumbnail that is already hovered is
    // drawn grown and lifted over that window, so the pixels under the pointer
    // are its own after all, and dropping the hover on the way across a covered
    // strip would have it shrink and grow again in the middle of itself.
    EffectWindow *hovered = nullptr;
    for (const auto &[w, state] : m_states) {
        if (state.hitRegion.contains(pos.toPoint())
            && (state.clicked || state.hovered || !m_shieldFilter.isCovered(w->window(), pos))
            && (!hovered || state.hovered)) {
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

    // The wider hold belongs to the click, and the click belongs to the visit:
    // the pointer leaving takes it with it, so the next arrival starts from the
    // resting rectangle again.
    if (!hovered) {
        it->second.clicked = false;
    }

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

void ThumbnailBloomEffect::engage(Window *window)
{
    // The thumbnail the input went through is the one under the pointer, and the
    // rest are left to updateHover(): the hit regions never overlap, so none of
    // them can be hovered at this moment anyway. Reduced motion holds this back
    // exactly as it holds back a pointer arriving the ordinary way.
    EffectWindow *const w = window ? window->effectWindow() : nullptr;
    if (w && !layoutFrozen()) {
        setHovered(w, true);
    }
}

void ThumbnailBloomEffect::standDown()
{
    // At once rather than by animation. The effect that has just taken the
    // screen begins its own animation from the real geometry on this very frame,
    // and a thumbnail sliding home across it is a second motion going somewhere
    // else; gone before the first frame, it is not seen at all.
    //
    // forget() is the ordinary end of a bloom, so this is the path a thumbnail
    // takes whenever it stops being one, only without the trip: the ground it
    // was painted over is repainted, its store is dropped, and its click target,
    // its shield and its canvas go with its state.
    //
    // Collected first, because forget() erases from the container.
    std::vector<EffectWindow *> bloomed;
    bloomed.reserve(m_states.size());
    for (const auto &[w, state] : m_states) {
        bloomed.push_back(w);
    }
    for (EffectWindow *w : bloomed) {
        forget(w);
    }

    // Run on the empty set, which is what hands every window its own input back:
    // the input windows come down and the filter is left claiming nothing, so a
    // press reaches whatever the effect now on the screen put there.
    updateInputWindows();
}

void ThumbnailBloomEffect::forget(EffectWindow *w)
{
    std::erase(m_liftedBelow.windows, w);
    std::erase(m_liftedAbove.windows, w);
    if (m_menuOwner == w) {
        m_menuOwner = nullptr;
        m_menuPopup = nullptr;
    }

    // Extracted rather than erased in place: destroying the store makes KWin
    // emit windowClosed for its internal window synchronously, and that handler
    // calls back into m_states, which has to be consistent by then. The store
    // itself only dies on the next event loop pass, because this can run under
    // input dispatch, where destroying an internal window is not survivable.
    // The click target and the shield belong to the screen and stay; the hit
    // region that goes with the state leaves their masks on the next pass.
    // The offscreen texture goes with the state. The window may already be gone
    // here (this also runs on windowClosed and windowDeleted), which is why the
    // flag is asked rather than the effect being told to unredirect blindly.
    const auto it = m_states.find(w);
    if (it != m_states.end()) {
        // Nothing is going to draw the thumbnail there again, so the ground it
        // was last painted over is repainted without it. Measured from the state
        // rather than from the window, which may be on its way out: this also
        // runs on windowClosed and windowDeleted.
        const QRectF ground = it->second.painted.united(it->second.base);
        if (!ground.isNull()) {
            effects->addRepaint(
                RectF(ground.adjusted(-paintMargin, -paintMargin, paintMargin, paintMargin)));
        }

        setSnapshot(w, it->second, false);
    }

    auto node = m_states.extract(w);
    if (!node.empty()) {
        if (ThumbnailCanvas *canvas = node.mapped().canvas.release()) {
            canvas->disconnect();
            canvas->deleteLater();
        }
    }
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

void ThumbnailBloomEffect::updateHitRegion(EffectWindow *w, BloomState &state)
{
    // The click target only claims what is actually visible of the thumbnail.
    // KWin hit tests an internal window against the mask of its QWindow, so
    // cutting the system elements out of that mask hands their own area back to
    // them: the panel keeps its hover feedback and its clicks, and so does every
    // popup that opens over a thumbnail.
    // A thumbnail claims its resting rectangle and no more, so that the pointer
    // can never be held by an area the thumbnail covers only because of that very
    // pointer. A click changes that: it went into the window, whatever it started
    // there is being aimed at the enlarged picture, and the pointer has to stay on
    // the thumbnail for as long as it is on what is drawn. The click target has to
    // grow with it and not merely answer for a wider area, because the moment the
    // pointer steps off it the compositor hands the focus to whatever is really
    // underneath, and the forwarded pointer goes with it.
    const QRectF claimed
        = state.clicked && !state.hoverRect.isEmpty() ? state.hoverRect : state.base;
    state.hitRegion = QRegion(claimed.toAlignedRect()) - m_systemRegion;

    // The growth reaches over the neighbouring thumbnails, and the overlap is
    // cut out of the neighbours: the grown thumbnail is drawn over them, and
    // where it is drawn is where its input belongs. A neighbour that ends up
    // covered whole loses its hit region for as long as the hold lasts, which
    // is right, since nothing of it can be seen. The pointer reaches it again
    // by leaving the grown rectangle, which is what ends the hold in the first
    // place. At most one thumbnail is ever held that way: the hold belongs to
    // the pointer's visit, and only one thumbnail is hovered at a time.
    if (!state.clicked) {
        for (const auto &[other, s] : m_states) {
            if (other != w && s.clicked && !s.hoverRect.isEmpty()) {
                state.hitRegion -= s.hoverRect.toAlignedRect();
            }
        }
    }

    // Both ends of a thumbnail's life: the trip back to its own window and the
    // dive into the point its screen collapses to. Neither leaves anything to
    // click.
    if (sameRect(state.base, frameRect(w)) || state.diving) {
        state.hitRegion = QRegion();
    }
}

void ThumbnailBloomEffect::setOwnWindowVisible(QWindow *window, bool visible)
{
    // Only ever reached from the relayout pass: showing and hiding internal
    // windows is not survivable under pointer dispatch or under the effect chain.
    if (window->isVisible() == visible) {
        return;
    }

    // Both restack, and KWin says so synchronously; the counter is what keeps
    // that from scheduling the very pass this is running in over again.
    ++m_ownWindowChange;
    window->setVisible(visible);
    // Hiding an internal window destroys the KWin::Window behind it, so every
    // show makes a fresh one with the default flags back and has to take it
    // out of the window lists again.
    if (visible) {
        hideFromWindowLists(window);
    }
    --m_ownWindowChange;
}

void ThumbnailBloomEffect::placeInputWindow(
    OverlayWindow *window, const QRect &screen, const QRegion &mask)
{
    if (mask.isEmpty()) {
        setOwnWindowVisible(window, false);
        return;
    }

    // The geometry is the screen's and changes with nothing else, so the buffer
    // behind the window is allocated and uploaded once per screen change rather
    // than once per relayout. The mask is what moves, and KWin reads it live in
    // its hit test without a buffer or a damage of its own.
    if (window->geometry() != screen) {
        window->setGeometry(screen);
    }
    setOverlayMask(window, mask.translated(-screen.topLeft()));
    setOwnWindowVisible(window, true);
}

void ThumbnailBloomEffect::updateInputWindows()
{
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
        thumbnailAreas.append(
            ShieldFilter::Thumbnail { w->window(), state.hitRegion, state.hoverRect });
    }

    // Walking the stack top down keeps a shield inside the area where its window
    // really is the topmost input target: everything above it has been added to
    // `covered` by the time the window is reached. Covering a window that lies
    // over a bloomed one would take away input that rightfully belongs to it.
    // What the thumbnails claim goes in at the start rather than being added to
    // every answer: it is the only thing `covered` is ever asked for, so the two
    // are the same region.
    QRegion covered = m_systemRegion + thumbnails;
    QRegion shieldRegion;
    QSet<Window *> bloomedWindows;
    QSet<Window *> backdropWindows;
    const QList<EffectWindow *> stack = effects->stackingOrder();
    for (auto it = stack.crbegin(); it != stack.crend(); ++it) {
        EffectWindow *w = *it;
        if (!isInputTarget(w) || isOwnOverlay(w)) {
            continue;
        }

        // Gathered on this walk rather than kept from the layout: the filter has
        // to be told about them on every pass anyway, and a set of windows the
        // effect held on to would outlive the ones that get closed.
        if (isBackdrop(w)) {
            backdropWindows.insert(w->window());
        }

        const QRect frame = w->frameGeometry().toAlignedRect();
        const auto sit = m_states.find(w);

        // Where the window is drawn is what marks it as bloomed, not what its
        // thumbnail has left to click: one can be left with no hit region at
        // all (buried under a panel, or covered whole by a grown neighbour) and
        // is still painted away from its own geometry, so its real place still
        // has to be shielded. Same test as updateHitRegion()'s.
        if (sit != m_states.end() && !sit->second.diving
            && !sameRect(sit->second.base, frameRect(w))) {
            // Every bloomed window has to be skipped when the input is handed
            // on, shielded or not: one that is covered everywhere still has to
            // stay out of the way under somebody else's shield.
            bloomedWindows.insert(w->window());
            shieldRegion += QRegion(frame) - covered;
        }

        // The bloomed window takes part as well: it is shielded where it is
        // exposed, so a window below must not claim that area either.
        covered += frame;
    }

    // One click target and one shield per screen, each cut to its screen. A
    // screen that has gone takes its two windows with it; one that has nothing
    // of either kind keeps them hidden.
    const QList<LogicalOutput *> screens = effects->screens();
    for (auto it = m_input.begin(); it != m_input.end();) {
        if (screens.contains(it->first)) {
            ++it;
            continue;
        }
        for (OverlayWindow *window :
            { static_cast<OverlayWindow *>(it->second.target.release()),
                it->second.shield.release() }) {
            if (window) {
                window->disconnect();
                ++m_ownWindowChange;
                delete window;
                --m_ownWindowChange;
            }
        }
        it = m_input.erase(it);
    }
    for (LogicalOutput *screen : screens) {
        ScreenInput &input = m_input[screen];
        const QRect geometry = screen->geometry();
        if (!input.target) {
            input.target = std::make_unique<ThumbnailOverlay>();
            // Which thumbnail a gesture is on is answered from the hit regions,
            // which is what the mask is the union of.
            input.target->setResolver([this](const QPointF &pos) -> QObject * {
                for (const auto &[w, state] : m_states) {
                    if (state.hitRegion.contains(pos.toPoint())) {
                        return w;
                    }
                }
                return nullptr;
            });
            connect(input.target.get(), &ThumbnailOverlay::activated, this,
                [](QObject *target) { effects->activateWindow(static_cast<EffectWindow *>(target)); });
            connect(input.target.get(), &ThumbnailOverlay::dragStarted, this,
                [this](QObject *target, const QPointF &pos, qint32 touchId) {
                    startThumbnailMove(static_cast<EffectWindow *>(target), pos, touchId);
                });
            // The menu command does not activate the window, which is the point:
            // a right click is a question about the thumbnail, not a use of it.
            connect(input.target.get(), &ThumbnailOverlay::menuRequested, this,
                [this](QObject *target, const QPointF &pos) {
                    openWindowMenu(static_cast<EffectWindow *>(target), pos);
                });
        }
        if (!input.shield) {
            input.shield = std::make_unique<OverlayWindow>();
        }
        placeInputWindow(input.target.get(), geometry, thumbnails & geometry);
        placeInputWindow(input.shield.get(), geometry, shieldRegion & geometry);
    }

    m_shieldFilter.setState(shieldRegion, bloomedWindows, thumbnailAreas, backdropWindows);
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

// Every surface of the effect's own is an OverlayWindow and nothing else in the
// process is, so the type answers this and no register of handles has to be kept
// in step with the states. A client window has no QWindow at all, whatever
// process it came from, so the whole of the stack but the internal windows is
// answered by the null check.

bool ThumbnailBloomEffect::isOwnOverlay(EffectWindow *w) const
{
    return qobject_cast<OverlayWindow *>(w->internalWindow()) != nullptr;
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

bool ThumbnailBloomEffect::isIgnored(
    EffectWindow *w, bool maximized, const QSet<EffectWindow *> &parents) const
{
    if (m_skipKeepAbove && w->keepAbove()) {
        return true;
    }
    if (m_skipOnAllDesktops && w->isOnAllDesktops()) {
        return true;
    }
    if (m_skipMaximized && maximized) {
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

void ThumbnailBloomEffect::updateBackdropScreens(const std::vector<EffectWindow *> &relevant,
    const std::vector<bool> &ignored, const std::vector<bool> &maximized)
{
    // Kept for the diff at the end: a screen changing its mind is what makes
    // every thumbnail on it appear or disappear at once.
    const QSet<LogicalOutput *> previous = m_backdropScreens;
    m_backdropScreens.clear();

    // Top down (`relevant` runs bottom up), the first window that speaks for its
    // screen settling it and the rest of that screen being skipped over. A
    // window the settings exempt says nothing, since a keep-above note or a
    // window kept on every desktop is not what the screen is being used for.
    // A maximized one is the exception to that exception: "skip maximized"
    // exempts exactly the windows this question is about, and passing them over
    // would answer with whatever they cover and make every maximized window in
    // front a backdrop.
    QSet<LogicalOutput *> settled;
    for (size_t i = relevant.size(); i-- > 0;) {
        EffectWindow *w = relevant[i];
        if (ignored[i] && !maximized[i]) {
            continue;
        }

        LogicalOutput *screen = w->screen();
        if (settled.contains(screen)) {
            continue;
        }
        settled.insert(screen);

        // A maximized window in front is what the user asked to look at, so
        // nothing is laid over it; anything else means the maximized windows
        // below it are only in the way.
        //
        // The window that speaks without being maximized is also the one the
        // thumbnails of this screen come out of and go back into, so its centre
        // is kept. A maximized speaker leaves the old point standing: the
        // thumbnails that are about to disappear belong to the arrangement it
        // replaced, and that is where they came from.
        if (!maximized[i]) {
            m_backdropScreens.insert(screen);
            m_screenFocus[screen] = frameRect(w).center();
        }
    }

    // The window being worked in has the last word on its own screen: while the
    // maximized one is the active window it is what the user is looking at,
    // whatever ended up stacked over it, so that screen shows no thumbnails over
    // it either. The screens it is not on are left as the walk decided them.
    EffectWindow *const active = effects->activeWindow();
    if (active && active->screen() && isMaximized(active)) {
        m_backdropScreens.remove(active->screen());
    }

    // A screen out of the exception has a maximized window across the whole of
    // its work area, which blocks every placement there is, so gaining and
    // losing the exception is gaining and losing every thumbnail of that screen.
    // That is the one change the burst animation is for.
    m_burstScreens = m_backdropScreens;
    m_burstScreens.subtract(previous);
    m_diveScreens = previous;
    m_diveScreens.subtract(m_backdropScreens);

    // The first layout has nothing to compare against: the windows it finds were
    // already on screen, so nothing about them just appeared.
    if (!m_backdropsSettled) {
        m_backdropsSettled = true;
        m_burstScreens.clear();
        m_diveScreens.clear();
    }
}

QPointF ThumbnailBloomEffect::burstPoint(LogicalOutput *screen) const
{
    if (!screen) {
        return QPointF();
    }
    return m_screenFocus.value(screen, QRectF(effects->clientArea(MaximizeArea, screen)).center());
}

bool ThumbnailBloomEffect::isBackdrop(EffectWindow *w) const
{
    return m_backdropScreens.contains(w->screen()) && isMaximized(w);
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

void ThumbnailBloomEffect::setSnapshot(EffectWindow *w, BloomState &state, bool wanted)
{
    // The software scene has no textures to hand out and no shaders to draw
    // them with, so every thumbnail there is drawn the ordinary way: flat, and
    // sampled once per pixel, as the whole effect was before.
    wanted = wanted && effects->isOpenGLCompositing();
    if (state.snapshot == wanted) {
        return;
    }

    state.snapshot = wanted;
    if (wanted) {
        // A window whose real place is buried under everything else still has to
        // be rendered, since the store is drawn from what the scene draws. This
        // is what says so, and it is dropped along with the store.
        state.item = ItemEffect(w->windowItem());
        state.stale = true;
        return;
    }

    // The texture is the size of the whole window and every bloomed window has
    // one, so it goes as soon as it stops being drawn from. Freeing a texture is
    // a call into the driver like any other and needs the context that made it.
    if (state.texture || state.fbo) {
        if (!EglContext::currentContext()) {
            effects->makeOpenGLContextCurrent();
        }
        state.fbo.reset();
        state.texture.reset();
    }
    state.item = ItemEffect();
}

GLShader *ThumbnailBloomEffect::filterShader()
{
    // Built once, on the first frame that draws a thumbnail, since compiling a
    // shader wants a current context and the effect is made long before there
    // is one. A failure is remembered rather than tried again every frame: the
    // shader leans on a few things an old scene may not have (the size of a
    // texture, the derivatives of a coordinate, KWin's own colour sources), and
    // a thumbnail is drawn from the mip chain instead when it cannot be had.
    if (!m_filterShaderBuilt) {
        m_filterShaderBuilt = true;
        m_filterShader = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture
                | ShaderTrait::Modulate | ShaderTrait::AdjustSaturation
                | ShaderTrait::TransformColorspace,
            QByteArray(), QByteArray(filterFragmentSource));
    }
    return m_filterShader.get();
}

bool ThumbnailBloomEffect::refreshSnapshot(EffectWindow *w, BloomState &state)
{
    // Everything the window paints, its shadow and its decoration along with it,
    // at the size the screen it lives on draws it. The thumbnail is only ever
    // smaller than that, so every pixel of the thumbnail comes out of a picture
    // that holds more detail than it can show.
    const qreal scale = deviceScale(w);
    const QRectF content = snapToPixels(QRectF(w->expandedGeometry()), scale);
    const QSize size = (content.size() * scale).toSize();
    if (size.isEmpty()) {
        state.fbo.reset();
        state.texture.reset();
        return false;
    }

    if (!state.texture || state.texture->size() != size) {
        state.texture = GLTexture::allocate(GL_RGBA8, size, mipLevels(size));
        if (!state.texture) {
            state.fbo.reset();
            return false;
        }

        state.texture->setFilter(GL_LINEAR_MIPMAP_LINEAR);
        state.texture->setWrapMode(GL_CLAMP_TO_EDGE);
        state.fbo = std::make_unique<GLFramebuffer>(state.texture.get());
        state.stale = true;
    }

    if (!state.stale) {
        return true;
    }
    state.stale = false;

    // The scene draws the window into the store exactly as it would draw it onto
    // the screen: untransformed, at its own size and its own place, with the
    // viewport standing where the window does.
    RenderTarget target(state.fbo.get());
    RenderViewport viewport(content, scale, target, QPoint());
    GLFramebuffer::pushFramebuffer(state.fbo.get());
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);

    WindowPaintData data;
    data.setOpacity(1.0);
    effects->drawWindow(target, viewport, w, PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT,
        Region::infinite(), data);

    GLFramebuffer::popFramebuffer();

    // Every level is halved from the one above it rather than made from the
    // window again, so the chain costs a third of the draw that has just
    // happened, and both only when the window has actually changed.
    state.texture->bind();
    state.texture->generateMipmaps();
    state.texture->unbind();
    return true;
}

void ThumbnailBloomEffect::paintSnapshot(const RenderTarget &renderTarget,
    const RenderViewport &viewport, EffectWindow *w, BloomState &state, const Region &deviceRegion,
    const WindowPaintData &data, const WindowQuadList &quads)
{
    // The filtering shader is the scene's own with the sampling replaced, so
    // either of the two composes the store exactly as the window itself would
    // have been: the same modulation, the same saturation and the same colour
    // space, whatever the screen it is going onto turns out to want. They take
    // the same uniforms for that reason.
    //
    // Which of the two draws this frame is a question of whether the thumbnail
    // is standing still. Weighing every pixel out of the picture texel by texel
    // is worth its cost on a thumbnail that is being looked at, where a
    // crawling edge or a letter coming out thick in one place and thin in the
    // next is plain to see; it is worth nothing on one crossing the screen,
    // which moves too fast for any of that to be made out, and the steps of a
    // trip are exactly where the cost tells, each of them being a repaint. So a
    // trip is drawn with the stock shader, which takes one sample off the mip
    // chain the store carries anyway: soft, but steady, and a single tap.
    //
    // The sharp filter comes back on the step the trip ends on rather than a
    // frame later, since advanceAnimations() damages that step like any other
    // and the timeline is already done by the time it is painted. Both ends of
    // a hover are such an arrival, the grown rectangle under the pointer as
    // much as the resting one.
    GLShader *shader = state.timeline.done() ? filterShader() : nullptr;
    if (!shader) {
        shader = ShaderManager::instance()->shader(ShaderTrait::MapTexture | ShaderTrait::Modulate
            | ShaderTrait::AdjustSaturation | ShaderTrait::TransformColorspace);
    }
    ShaderBinder binder(shader);

    const double scale = viewport.scale();

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));

    // Never snapped to the pixel grid, which is what a RenderGeometry does by
    // itself and what OffscreenEffect::setVertexSnappingMode() exists to turn
    // off. These vertices are in the window's own coordinates and the matrix
    // below is what puts them on the screen, so rounding them lines nothing up
    // with anything: it only drags each corner of the bend grid up to half a
    // window pixel away from where the perspective put it, while its texture
    // coordinate stays behind, and neighbouring cells round different ways.
    // What the thumbnail must land on whole pixels is its rectangle, which
    // retarget() has already rounded.
    RenderGeometry geometry;
    geometry.setVertexSnappingMode(RenderGeometry::VertexSnappingMode::None);
    for (const WindowQuad &quad : quads) {
        geometry.appendWindowQuad(quad, scale);
    }
    geometry.postProcessTextureCoordinates(state.texture->matrix(NormalizedCoordinates));

    const auto map = vbo->map<GLVertex2D>(geometry.size());
    if (!map) {
        return;
    }
    geometry.copy(*map);
    vbo->unmap();
    vbo->bindArrays();

    // The vertices are in window coordinates, at the window's own size: what
    // makes a thumbnail of them is the matrix, which carries the scale and the
    // translation applyTransform() put on the paint data.
    const qreal rgb = data.brightness() * data.opacity();
    const qreal alpha = data.opacity();

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(std::round(w->x() * scale), std::round(w->y() * scale));

    const auto toXYZ = renderTarget.colorDescription()->containerColorimetry().toXYZ();
    shader->setUniform(
        GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp * data.toMatrix(scale));
    shader->setUniform(GLShader::Vec4Uniform::ModulationConstant, QVector4D(rgb, rgb, rgb, alpha));
    shader->setUniform(GLShader::FloatUniform::Saturation, data.saturation());
    shader->setUniform(
        GLShader::Vec3Uniform::PrimaryBrightness, QVector3D(toXYZ(1, 0), toXYZ(1, 1), toXYZ(1, 2)));
    shader->setUniform(GLShader::IntUniform::TextureWidth, state.texture->width());
    shader->setUniform(GLShader::IntUniform::TextureHeight, state.texture->height());
    shader->setColorspaceUniforms(
        ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

    const bool clipping = deviceRegion != Region::infinite();
    const Region clipRegion = clipping
        ? viewport.transform().map(deviceRegion, renderTarget.transformedSize())
        : Region::infinite();

    if (clipping) {
        glEnable(GL_SCISSOR_TEST);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    state.texture->bind();
    vbo->draw(clipRegion, GL_TRIANGLES, 0, geometry.count(), clipping);
    state.texture->unbind();

    glDisable(GL_BLEND);
    if (clipping) {
        glDisable(GL_SCISSOR_TEST);
    }
    vbo->unbindArrays();
}

QTransform ThumbnailBloomEffect::stateBend(
    EffectWindow *w, const BloomState &state, const QRectF &rect) const
{
    return bendTransform(
        rect, m_bendAngle * state.bend.current, bendDirection(w, state.rect.current));
}

QRectF ThumbnailBloomEffect::paintedArea(EffectWindow *w, const BloomState &state) const
{
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

    return bounds.adjusted(-paintMargin, -paintMargin, paintMargin, paintMargin);
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
    //
    // How fine the grid has to be is asked of the thumbnail rather than fixed at
    // the worst case. Every cell of it is built, mapped and streamed to the card
    // on every frame the thumbnail is drawn, so a grid finer than the picture can
    // show is paid for over and over; the error it is cut to hide falls as the
    // square of the cell count, which is why the count needed at the angles the
    // effect is used at is a fraction of the one the steepest setting wants. The
    // measure is the rectangle the thumbnail is drawn at, since the grid is cut
    // in the window's coordinates and everything in it is scaled down by exactly
    // that ratio on its way to the screen.
    const QRectF &drawn = bloomState.rect.current;
    const int cells = bendSubdivisions(std::max(drawn.width(), drawn.height()),
        m_bendAngle * bloomState.bend.current, deviceScale(window));
    if (cells > 1) {
        quads = quads.makeRegularGrid(cells, cells);
    }

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

    // A hair larger than the rectangle the layout gave it, so that the edge of
    // the picture runs under the inner edge of the frame instead of meeting it:
    // see thumbnailBleed. Only the drawing grows. Everything that measures a
    // thumbnail (the hit region, the lift, the damage) works from the rectangle
    // itself, which is what the frame is drawn around too.
    const QRectF drawn = state.rect.current.adjusted(
        -thumbnailBleed, -thumbnailBleed, thumbnailBleed, thumbnailBleed);

    // Scaling happens around the window's top left corner, so the translation is
    // expressed in unscaled screen coordinates.
    data.setScale(QVector2D(drawn.width() / natural.width(), drawn.height() / natural.height()));
    data.setXTranslation(drawn.x() - natural.x());
    data.setYTranslation(drawn.y() - natural.y());
    data.multiplyOpacity(state.opacity.current);
}

void ThumbnailBloomEffect::prePaintScreen(ScreenPrePaintData &data)
{
    // Read when the colour scheme says so rather than per pass, let alone per
    // thumbnail: building a KColorScheme means opening the scheme's config group
    // and computing a whole palette out of it, which is some thirty microseconds
    // of a frame that has sixteen thousand of them, and the answer changes only
    // when the user changes the scheme. The palette change reaches the
    // application object and nothing else in Qt 6, which is what the event filter
    // is for. Refreshed before the animations advance, since that is where each
    // frame is handed the colour it is at.
    if (m_outlineDirty) {
        m_outlineDirty = false;
        m_restOutline = restOutlineColor();
        m_hoverOutline = hoverOutlineColor();
        m_outlineSerial++;
    }

    const std::vector<EffectWindow *> settledBack = advanceAnimations(data);
    updateLift(data.screen);

    // The ground the animations have moved over since this screen last painted,
    // in logical coordinates, which is what the scene expects here and maps into
    // the damage of the pass itself. Widening the frame that is already
    // happening is what pays for it; postPaintScreen() then asks for the next
    // one, from wherever the thumbnails have got to by then.
    //
    // It is taken per screen and not per pass, because every screen paints a
    // pass of its own and the animations advance in each of them: the position a
    // screen last drew is one or two steps behind the one the last pass of some
    // other screen left, and a frame that erased only the latter would leave the
    // trailing edge of the thumbnail standing on it.
    //
    // An empty damage is left alone, and the ground is then kept rather than
    // taken: no pass is drawing anything, widening one would make a frame out of
    // a pass that was going to paint nothing at all, and nothing of what is owed
    // to this screen has been repainted.
    //
    // Nothing is asked for on behalf of the lifted thumbnails, which are drawn
    // out of turn after an anchor rather than at their own depth. What they need
    // is not damage of their own but that the anchor be painted whenever the
    // damage reaches them, and prePaintWindow() sees to that by marking it
    // transformed. A thumbnail that has come to rest under the pointer therefore
    // costs nothing: nothing on the screen is changing, so nothing is drawn.
    if (!data.paint.isEmpty()) {
        QRegion &pending = m_pending[data.screen];
        if (!pending.isEmpty()) {
            data.paint |= Region(pending);
            pending = QRegion();
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
    m_moved = QRegion();

    std::vector<EffectWindow *> settledBack;
    for (auto &[w, state] : m_states) {
        const QRectF before = state.painted;

        // Asked before the step rather than after: a timeline that was already
        // finished cannot move the thumbnail, and one that finishes on this step
        // has moved it as far as any other step did. A thumbnail resting for
        // minutes therefore adds nothing to the damage, which is what keeps the
        // repaint of an unrelated corner of the screen from redrawing every
        // thumbnail on it.
        const bool moving = !state.timeline.done();

        // A thumbnail whose trip was over before this step is exactly where the
        // last one left it, and so is everything drawn on it: every channel is
        // sitting on its destination and the store holds the frame those values
        // asked for. The whole step is therefore skipped rather than
        // interpolated, bent and handed to the store again, which is two
        // projective solves and a walk of four corners per thumbnail per pass and
        // comes to the same answer every time. Two things can move under a
        // thumbnail at rest without a trip being started: the colour scheme, and
        // the window itself, which the frame leans towards. Both are asked here,
        // and neither is the ordinary case.
        //
        // What the step would have added to the damage is nothing either: a
        // finished timeline cannot move the thumbnail, which is what the pass
        // below already says of it. The store has to have caught up first, since
        // the pass of another screen can have taken the last step of the trip
        // without repainting it.
        if (!moving && !state.canvasStale && state.outlineSerial == m_outlineSerial
            && frameRect(w).center() == state.bendOrigin) {
            continue;
        }

        state.timeline.advance(data.view);
        const qreal progress = state.timeline.value();
        state.rect.interpolate(progress);
        state.opacity.interpolate(progress);
        state.caption.interpolate(progress);
        state.bend.interpolate(progress);
        state.highlight.interpolate(progress);
        state.canvasStale = true;

        // The store of a thumbnail on another screen is left to the pass of that
        // screen, which runs this same walk and reaches it before anything draws
        // it. Repainting it here as well would draw every frame of every caption
        // and every frame twice over on a desk with two screens, and the second
        // of the two would be thrown away unlooked at.
        if (!data.screen || w->screen() == data.screen) {
            refreshCanvas(w, state);
        }

        state.painted = paintedArea(w, state);

        // Where the thumbnail was and where it now is, so that the frame erases
        // the one and draws the other. As two rectangles, not as the one around
        // both: nothing was ever drawn on the ground between them, since every
        // step's `before` is the `painted` of the step before it and the ledger
        // below carries every step a screen has missed, so there is nothing
        // there to erase. The rectangle around both is most of the screen for a
        // thumbnail crossing it, on every frame of the trip.
        //
        // The frame a trip ends on is measured like every other one, and before
        // the state is dropped rather than after: that step moves the thumbnail
        // as far as the one before it did, and what it leaves behind is painted
        // by this pass or by nothing at all.
        if (moving) {
            m_moved += before.toAlignedRect();
            m_moved += state.painted.toAlignedRect();
            m_moved += state.base.toAlignedRect();
        }

        if (!state.timeline.done()) {
            continue;
        }

        // A thumbnail that has arrived back at its window is not a thumbnail
        // any more, and neither is one that has dived into the point: there is
        // nothing left of it to draw, and the window it belongs to is under
        // whatever took the screen over. Which trip it was on is what decides
        // this, not where it ended up: a window being dragged has moved on since
        // the thumbnail was last aimed at it, and comparing the two would hold a
        // finished trip open frame after frame. Click targets are placed by the
        // relayout, not from here.
        //
        // A window held out of its bloom by a grab is the one arrival that is
        // not the end of anything: it has to keep its state for as long as the
        // grab lasts, since that state is what draws it over the windows
        // covering it, and dropping it here would put it straight back under
        // them. What it has no more use for is its store: the window is drawn
        // flat, at its own size and in its own place, so there is neither a bend
        // to draw nor a downscale to filter, and a texture the size of the
        // window is a lot to hold for that. The trip that got it there is over,
        // so letting go of it now shows nothing.
        if (state.blocked) {
            setSnapshot(w, state, false);
            continue;
        }

        if (state.diving || state.homing) {
            settledBack.push_back(w);
        }
    }

    // Owed to every screen, including the one about to paint: what a screen has
    // to repaint is the whole chain of steps since it last drew, and each step
    // covers the ground between two consecutive positions, so the union of them
    // covers every position the thumbnail was drawn at in between. The screens
    // that have gone are dropped, a region nobody will ever paint being carried
    // for ever otherwise.
    if (!m_moved.isEmpty()) {
        const QList<LogicalOutput *> screens = effects->screens();
        std::erase_if(
            m_pending, [&screens](const auto &entry) { return !screens.contains(entry.first); });
        for (LogicalOutput *screen : screens) {
            m_pending[screen] += m_moved;
        }
    }

    return settledBack;
}

void ThumbnailBloomEffect::updateLift(LogicalOutput *screen)
{
    // A thumbnail is lifted while the pointer has it grown, and stays lifted
    // until it has shrunk all the way back: the hover is over the moment the
    // pointer leaves, but the way back takes a whole animation, and dropping the
    // thumbnail behind its neighbours at the first frame of it is a visible jump.
    // The size it is drawn at is the second half of the test, so no timeline has
    // to be consulted: between the hover ending and the relayout that retargets
    // the animation the timeline is still the finished one of the way up.
    //
    // The first half is retarget()'s flag, which asks which trip the thumbnail is
    // on: the pointer's and the picked window's way home are lifted, every other
    // one is not. A thumbnail merely being relaid out is drawn over whatever the
    // stacking order says once it arrives, so it is drawn there on the way as
    // well, and the size test alone would lift every thumbnail whose new
    // rectangle is smaller than the one it is leaving.
    //
    // The active window on its way back to its real geometry is lifted by both: it
    // is the thumbnail that was just picked, growing past its resting rectangle,
    // and its trip crosses the thumbnails it is leaving behind.
    //
    // A thumbnail laid over a backdrop is lifted for as long as it is one, size
    // or no size: its own place in the stacking order is under the backdrop, so
    // drawing it there would not show it at all. The anchor walk below then finds
    // that backdrop as the topmost window covering it and draws it right after.
    //
    // A thumbnail diving into the burst point is lifted for the whole of that
    // trip as well, and into the moving group whatever its size: it is
    // shrinking, so the size test never reaches it, and the window it is diving
    // into is the one that has just taken the screen over, which is exactly the
    // window the resting group is kept under. That is the one place where a
    // lifted thumbnail is not being enlarged.
    //
    // A window held out of its bloom by a grab of its own is lifted for as long
    // as it is held, and lifted into the moving group whatever its size: what it
    // put up is a popup, painted above every ordinary window there is, so a
    // window left at its own depth would have its menu standing over the very
    // windows that hide it. Drawn after the topmost window covering it, it comes
    // out under its own popups and over everything else, which is the whole
    // point of showing it in place. The active window is no exception here: it
    // is the likeliest thing to be covering the window that was asked a question.
    //
    // They are ordered by that same size, most enlarged last, so the thumbnails
    // still on their way up cover the ones already on their way down.
    //
    // The set is then split in two, and only a thumbnail that is being resized
    // right now may be drawn over the active window: that window is the one
    // another effect is most likely to be animating (a minimise, a slide, a
    // wobble), and a thumbnail parked over it would be painted where that
    // animation is. One in motion is the exception, because the growing and the
    // shrinking are the same gesture and dropping the thumbnail behind the
    // active window halfway through it is the visible jump the lift exists to
    // avoid. Both trips end at the resting size, where the thumbnail either
    // stops being lifted at all or falls back into the group below.
    //
    // The active window itself goes with them. While it is in this set at all it
    // is the thumbnail that was just picked, travelling home, and nothing may be
    // drawn over it; keeping it under itself would mean nothing anyway. It sorts
    // last of all, and it is only ever here for the length of that trip, so this
    // never lifts an ordinary focused window out of the stacking order.
    //
    // Both groups hold the thumbnails of \a screen alone, and only the windows
    // painted in this pass can anchor them: the pass draws one screen, so a
    // group anchored to a window of another one would never be drawn at all.
    EffectWindow *const active = effects->activeWindow();
    const QRectF screenArea = screen ? QRectF(screen->geometry()) : QRectF();
    const auto inPass = [screen, &screenArea](EffectWindow *w) {
        return !screen || w->frameGeometry().intersects(screenArea);
    };
    m_liftedBelow.windows.clear();
    m_liftedAbove.windows.clear();
    for (const auto &[w, state] : m_states) {
        if (screen && w->screen() != screen) {
            continue;
        }
        const bool resizing = state.lift != Lift::None
            && growth(state.rect.current, state.thumbBase) > 1.0 + liftEpsilon;
        const bool diving = state.lift == Lift::Dive;
        if (state.blocked || state.hovered || state.overBackdrop || resizing || diving) {
            LiftGroup &group = state.blocked || state.hovered || resizing || diving || w == active
                ? m_liftedAbove
                : m_liftedBelow;
            group.windows.push_back(w);
        }
    }
    const auto leastEnlarged = [this, active](EffectWindow *a, EffectWindow *b) {
        const BloomState &sa = m_states.at(a);
        const BloomState &sb = m_states.at(b);
        // A window held out of its bloom is no thumbnail at all: it is drawn at
        // its own size, in its own place, and everything the lift is really
        // about belongs over it. It sorts before the lot, the active window
        // included, since the only trip that brings that one here is its way
        // home from a thumbnail.
        if (sa.blocked != sb.blocked) {
            return sa.blocked;
        }
        if (a == active || b == active) {
            return b == active && a != active;
        }
        return growth(sa.rect.current, sa.thumbBase) < growth(sb.rect.current, sb.thumbBase);
    };
    std::ranges::sort(m_liftedBelow.windows, leastEnlarged);
    std::ranges::sort(m_liftedAbove.windows, leastEnlarged);

    // Each group is drawn right after the topmost window that covers any of its
    // thumbnails, and not after the whole screen: everything painted later (the
    // popup layer the outlines live in, and the cursor) keeps painting over them.
    //
    // Covering is measured against what each window puts on the screen rather
    // than against the rectangle it occupies (footprint(), below), which for a
    // bloomed window is its thumbnail and the rectangle its caption is painted
    // on. That is what makes the neighbour a thumbnail grew across an anchor:
    // the enlarged one has to cover its picture and its caption alike, and the
    // real window it belongs to is buried somewhere else and covers nothing. The
    // caption of the enlarged thumbnail is faded out under the pointer, so
    // nothing of it is lost the other way round.
    //
    // The anchor is then painted whatever this pass is repainting, since
    // prePaintWindow() marks it transformed: a window is otherwise left out of a
    // partial repaint that does not touch the rectangle it occupies, and the
    // whole group would go with it.
    //
    // When nothing covers any of them they are already the topmost windows, but
    // they still have to be drawn in one place to be ordered among themselves,
    // so the topmost of the group becomes its own anchor and the set takes its
    // turn there.
    m_liftedBelow.anchor = nullptr;
    m_liftedAbove.anchor = nullptr;
    if (!m_liftedBelow.windows.empty() || !m_liftedAbove.windows.empty()) {
        std::vector<QRect> passedBelow; // thumbnails of each group already under the walk
        std::vector<QRect> passedAbove;
        EffectWindow *fallbackBelow = nullptr; // topmost of the group, anchor or no anchor
        int index = 0, belowIndex = -1, aboveIndex = -1, fallbackIndex = -1;

        const auto covers = [](const std::vector<QRect> &passed, const QRect &frame) {
            return std::ranges::any_of(passed, [&](const QRect &r) { return frame.intersects(r); });
        };

        // What a window puts on the screen, which is what covering means here.
        // For a bloomed one that is its thumbnail and the caption on the
        // rectangle the layout gave it, never the rectangle it really occupies:
        // a thumbnail is drawn over the neighbour it grew across, and the real
        // window behind that neighbour is buried somewhere else entirely and
        // covers nothing. Measured against the real rectangle, the neighbour
        // would not be recognised as covering anything, the group would be
        // anchored below it, and the neighbour's own thumbnail, outline and
        // caption would then be painted over the enlarged one.
        const auto footprint = [this](EffectWindow *w) -> QRect {
            const auto it = m_states.find(w);
            if (it != m_states.end()) {
                return it->second.painted.united(it->second.base).toAlignedRect();
            }
            return w->frameGeometry().toRect();
        };

        for (EffectWindow *w : effects->stackingOrder()) {
            ++index;

            // The stacking order runs bottom to top, so only what follows covers
            // the thumbnail; anything below it is already covered.
            if (isLifted(m_liftedBelow, w)) {
                passedBelow.push_back(footprint(w));
                fallbackBelow = w;
                fallbackIndex = index;
                m_liftedBelow.anchor = w;
                belowIndex = index;
            } else if (isLifted(m_liftedAbove, w)) {
                passedAbove.push_back(footprint(w));
                m_liftedAbove.anchor = w;
                aboveIndex = index;
            } else if (passedBelow.empty() && passedAbove.empty()) {
                // Nothing lifted has been passed yet, so nothing here can be
                // covering one: the stack runs bottom to top. Worth saying
                // outright, since everything below the lowest lifted thumbnail
                // would otherwise be measured and classified for nothing, on
                // every frame of every pass.
                continue;
            } else if (m_relevantWindows.contains(w) && inPass(w)) {
                const QRect frame = footprint(w);

                // The resting group stops short of the active window itself,
                // which is the whole point of the split. Only of that one
                // window: the windows above it are what hides the thumbnail
                // (the backdrop it is laid over among them), and a group held
                // under those would not be seen at all. Whatever is painted
                // between them and the active window is hidden anyway, so the
                // animation the split protects is out of sight there either way.
                if (w != active && covers(passedBelow, frame)) {
                    m_liftedBelow.anchor = w;
                    belowIndex = index;
                }
                if (covers(passedAbove, frame)) {
                    m_liftedAbove.anchor = w;
                    aboveIndex = index;
                }
            }
        }

        // Nothing on this screen covers them, so they are drawn at the place of
        // their topmost member and are only ordered among themselves.
        if (!m_liftedBelow.anchor) {
            m_liftedBelow.anchor = fallbackBelow;
            belowIndex = fallbackIndex;
        }

        // A thumbnail in motion covers the resting ones whatever the stack says,
        // so its group can never be drawn before theirs. Sharing the anchor is enough:
        // the paint pass runs the two groups in order at the same window.
        if (m_liftedAbove.anchor && aboveIndex < belowIndex) {
            m_liftedAbove.anchor = m_liftedBelow.anchor;
        }
    }
    m_liftedBelow.pending = m_liftedBelow.anchor != nullptr;
    m_liftedAbove.pending = m_liftedAbove.anchor != nullptr;
}

void ThumbnailBloomEffect::prePaintWindow(
    RenderView *view, EffectWindow *w, WindowPrePaintData &data)
{
    const auto it = m_states.find(w);
    if (it != m_states.end()
        && (it->second.blocked || !sameRect(it->second.rect.current, frameRect(w))
            || it->second.opacity.current < 1.0)) {
        // The window is painted somewhere else and at another size, so it may
        // neither be clipped against nor culled by its real geometry.
        //
        // A blocked window is painted at exactly its real geometry, but out of
        // turn: the windows above it would otherwise cull it out of the pass
        // altogether, and the draw that comes after their anchor would have
        // nothing left to work with.
        data.setTransformed();
        data.setTranslucent();
    }

    // The anchor of a lift group is where the thumbnails of that group are
    // drawn, so it may not be left out of the pass on account of its own
    // geometry: an ordinary window is painted only where the damage meets the
    // rectangle it occupies, and a repaint that touches a lifted thumbnail need
    // touch nothing of the window it is drawn over. Marked transformed, the
    // anchor is painted whenever anything of the damage is left under it at all,
    // which is exactly when the group has something to draw; where the damage is
    // covered by opaque windows above the anchor, nothing drawn after it could
    // be seen anyway. The cost is that it stops occluding the windows below it,
    // which is one window's worth of overdraw inside the damage and no more.
    if (w == m_liftedBelow.anchor || w == m_liftedAbove.anchor) {
        data.setTransformed();
    }

    Effect::prePaintWindow(view, w, data);
}

void ThumbnailBloomEffect::paintWindow(const RenderTarget &renderTarget,
    const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion,
    WindowPaintData &data)
{
    // Every surface of the effect's own is left out here. A store is an internal
    // window, and KWin puts every one of those in the topmost layer, so painting
    // it where it is in the stack would show the frame and the caption over
    // whatever covers the thumbnail; it is drawn again right after the window it
    // belongs to instead, which is what gives both of them the depth of their own
    // thumbnail. From there the compositor covers picture, frame and caption
    // together, and nothing has to be worked out from geometry.
    //
    // The click targets and the shields are never drawn again at all. They take
    // input and paint nothing, and a window that paints nothing is still a
    // transparent quad the scene blends over the damage on every frame.
    if (isOwnOverlay(w)) {
        return;
    }

    // The lifted thumbnails are left out at their own places in the stacking
    // order and drawn again after the anchor of their group: not chaining the
    // call is what keeps a window out of a pass. An anchor may be a lifted
    // thumbnail itself, and then its own turn is where its set is drawn.
    if (!isLifted(w)) {
        const auto it = m_states.find(w);
        // The region the window is painted with is the clip of the frame and the
        // caption alike: it is the damage of the pass less what opaque windows
        // above this one cover, so what is drawn here reaches exactly as far as
        // the thumbnail itself does. Taking the damage of the whole pass instead
        // would draw over ground this window is buried under, and taking no
        // region at all would draw over pixels the frame never cleared.
        //
        // The frame goes down after the thumbnail and before the caption. Over
        // the thumbnail rather than under it, although it is drawn outside the
        // rectangle it frames: the shadow of a window reaches past that
        // rectangle and is painted with the thumbnail, so a line underneath
        // would be tinted by it. Over the picture it also covers the half pixel
        // the thumbnail is bled outwards by, which is what leaves no seam at the
        // edge.
        if (it != m_states.end()) {
            applyTransform(w, it->second, data);
        }

        Effect::paintWindow(renderTarget, viewport, w, mask, deviceRegion, data);

        if (it != m_states.end()) {
            drawCanvas(renderTarget, viewport, it->second, deviceRegion);
        }
    }

    // Always in this order, which is what puts the hovered thumbnail over the
    // rest even when the two groups share an anchor.
    if (w == m_liftedBelow.anchor) {
        drawLifted(renderTarget, viewport, m_liftedBelow, deviceRegion);
    }
    if (w == m_liftedAbove.anchor) {
        drawLifted(renderTarget, viewport, m_liftedAbove, deviceRegion);
    }
}

void ThumbnailBloomEffect::drawWindow(const RenderTarget &renderTarget,
    const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion,
    WindowPaintData &data)
{
    const auto it = m_states.find(w);
    if (it == m_states.end() || !it->second.snapshot || !refreshSnapshot(w, it->second)) {
        Effect::drawWindow(renderTarget, viewport, w, mask, deviceRegion, data);
        return;
    }

    // One quad over everything the window paints, in window coordinates: the
    // frame geometry at the origin, the shadow and the decoration reaching
    // outside it. It covers the store corner for corner, and apply() cuts it
    // into the grid the bend needs.
    const QRectF expanded = snapToPixels(QRectF(w->expandedGeometry()), viewport.scale());
    const QRectF frame = snapToPixels(QRectF(w->frameGeometry()), viewport.scale());
    const QRectF visible(expanded.topLeft() - frame.topLeft(), expanded.size());

    WindowQuad quad;
    quad[0] = WindowVertex(visible.topLeft(), QPointF(0, 0));
    quad[1] = WindowVertex(visible.topRight(), QPointF(1, 0));
    quad[2] = WindowVertex(visible.bottomRight(), QPointF(1, 1));
    quad[3] = WindowVertex(visible.bottomLeft(), QPointF(0, 1));

    WindowQuadList quads;
    quads.append(quad);
    apply(w, mask, data, quads);

    paintSnapshot(renderTarget, viewport, w, it->second, deviceRegion, data, quads);
}

bool ThumbnailBloomEffect::isLifted(EffectWindow *w) const
{
    return isLifted(m_liftedBelow, w) || isLifted(m_liftedAbove, w);
}

bool ThumbnailBloomEffect::isLifted(const LiftGroup &group, EffectWindow *w)
{
    return std::ranges::find(group.windows, w) != group.windows.end();
}

void ThumbnailBloomEffect::drawLifted(const RenderTarget &renderTarget,
    const RenderViewport &viewport, LiftGroup &group, const Region &deviceRegion)
{
    if (!group.pending) {
        return;
    }
    group.pending = false;

    // Least enlarged first: the set is ordered that way, so the thumbnail the
    // pointer is growing ends up over the ones it is leaving behind.
    for (EffectWindow *w : group.windows) {
        const auto it = m_states.find(w);
        if (it == m_states.end()) {
            continue;
        }

        // The region is the clip of the draw, and it is the one the anchor was
        // painted with: the damage of the pass less what opaque windows above
        // the anchor cover, which is precisely the ground a thumbnail drawn
        // after it can be seen on. The anchor is marked transformed in
        // prePaintWindow(), so that region is not cut down to the rectangle the
        // anchor itself occupies. An infinite region would have the opposite
        // problem, since painting outside the damage blends the translucent
        // parts of the thumbnail over pixels that were never cleared, and the
        // shadow would darken frame by frame.
        WindowPaintData data;
        applyTransform(w, it->second, data);

        effects->drawWindow(renderTarget, viewport, w,
            PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT, deviceRegion, data);

        // The store follows the thumbnail rather than the lift: every one of them
        // is framed and captioned, and the highlight channel only decides how
        // thick the line is and in what colour. See refreshCanvas(). Drawn here
        // so that the frame and the caption of a lifted thumbnail end up over it
        // rather than under it, as they do everywhere else, and out of the reach
        // of its shadow.
        drawCanvas(renderTarget, viewport, it->second, deviceRegion);
    }
}

void ThumbnailBloomEffect::drawCanvas(const RenderTarget &renderTarget,
    const RenderViewport &viewport, BloomState &state, const Region &deviceRegion)
{
    // Kept from the relayout that put the store up rather than looked up here:
    // this runs for every bloomed window of every frame, and the lookup walks the
    // internal windows. The pointer empties itself if the window goes away behind
    // the effect's back, and the lookup is done again then rather than the
    // thumbnail losing its frame for the frame.
    if (!state.canvasWindow && state.canvas) {
        state.canvasWindow = effects->findWindow(state.canvas.get());
    }

    EffectWindow *overlay = state.canvasWindow;
    if (!overlay || !overlay->isVisible()) {
        return;
    }

    // Normally nothing is transformed here, which is the whole point of the
    // store: refreshCanvas() has moved it onto the thumbnail and painted the line
    // and the caption into it at the size this very frame draws, in this very
    // turn, so the window is drawn where it is and at the size it is. Scaling it
    // is what used to soften the line on a thumbnail the pointer had grown, and
    // the smaller the thumbnail the further it was scaled.
    //
    // What the store holds is asked all the same, rather than assumed: a paint
    // can be missed (a window not yet exposed, a geometry KWin has not committed
    // yet), and a frame of the thumbnail drawn with the store of the frame before
    // is a line lagging behind its thumbnail. So the buffer is put where it
    // belongs whatever is in it, which at worst is the step before this one and
    // a scale of a few hundredths.
    WindowPaintData data;
    bool corrected = false;
    const QPointF origin = overlay->frameGeometry().topLeft();
    const QRectF rect = state.rect.current;
    const QRectF shown = state.canvas ? state.canvas->shownRect() : QRectF();
    if (!rect.isEmpty() && !shown.isEmpty()) {
        const QRectF painted(origin + shown.topLeft(), shown.size());
        const auto near = [](qreal a, qreal b) { return std::abs(a - b) < outlineSlack; };
        if (!near(painted.x(), rect.x()) || !near(painted.y(), rect.y())
            || !near(painted.width(), rect.width()) || !near(painted.height(), rect.height())) {
            // Scale about the window's own corner, translation in unscaled
            // screen coordinates: the store's corner has to land where the
            // painted rectangle's corner is asked to go.
            const qreal scaleX = rect.width() / shown.width();
            const qreal scaleY = rect.height() / shown.height();
            data.setScale(QVector2D(scaleX, scaleY));
            data.setXTranslation(rect.x() - origin.x() - shown.x() * scaleX);
            data.setYTranslation(rect.y() - origin.y() - shown.y() * scaleY);
            corrected = true;
        }
    }

    // A store is the size of the thumbnail it belongs to and holds ink around
    // the edge of it alone: the band of the frame and the strip of the caption,
    // with the whole of the picture between them left untouched. Drawn as it
    // stands it is nonetheless a quad the size of the thumbnail, which the scene
    // blends pixel by pixel over the damage; that is the screen over again for
    // every thumbnail on it, nine tenths of it spent on pixels that are empty.
    // Clipping the draw to the ground the store was actually painted on hands
    // all of that back. Only where the store is drawn where it was painted: the
    // correction above moves the ink, and the region says where it was, not
    // where it is going.
    Region clip = deviceRegion;
    if (!corrected && clip != Region::infinite()) {
        const QRegion ink = state.canvas ? state.canvas->paintedRegion() : QRegion();
        if (ink.isEmpty()) {
            return;
        }
        clip &= viewport.mapToDeviceCoordinatesAligned(
            Region(ink.translated(origin.toPoint())));
        if (clip.isEmpty()) {
            return;
        }
    }

    effects->drawWindow(
        renderTarget, viewport, overlay, PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT, clip, data);
}

void ThumbnailBloomEffect::updateCanvas(EffectWindow *w, BloomState &state)
{
    // Only ever reached from the relayout pass, like the click target: hiding an
    // internal window makes KWin destroy it synchronously, which must not happen
    // under pointer dispatch or under the effect chain.
    //
    // A window travelling back to its own geometry stops being a thumbnail, but
    // it keeps its store for as long as there is any frame or caption left to
    // fade out; refreshCanvas() is what empties it, and the state is dropped once
    // the trip ends.
    const bool leaving = sameRect(state.base, frameRect(w)) || state.diving;
    if (leaving && state.caption.current <= 0.0 && state.highlight.current <= 0.0) {
        if (state.canvas) {
            setOwnWindowVisible(state.canvas.get(), false);
            state.canvasWindow = nullptr;
        }
        return;
    }

    // A dive has no rectangle to speak of: the thumbnail is shrinking into the
    // point its screen collapses to. A store already up keeps the size it has,
    // the frame inside it shrinking with the thumbnail; one that is not up has
    // nothing to be sized by and is not put up at all.
    if (!state.canvas && state.base.isEmpty()) {
        return;
    }

    if (!state.canvas) {
        state.canvas = std::make_unique<ThumbnailCanvas>();
    }

    // Everything the caption is made of is settled here rather than in
    // refreshCanvas(): the icon, the title and the size they are laid out for all
    // change when the window changes, which is a relayout, and the store drops the
    // picture it made of them whenever they do. The size is the resting one, so
    // that the caption is laid out once and carried along a trip rather than laid
    // out again for every size the picture passes through; laying it out against
    // the travelling rectangle would mean blurring its two shadows on every frame.
    state.canvas->setCaption(m_showIcons ? w->icon() : QIcon(),
        m_showTitles ? w->caption() : QString(), state.base.size());

    // Where the store goes and how large it is are settled by refreshCanvas(),
    // which runs for every frame of every animation; it is called here so that a
    // store put up for the first time is already on its thumbnail when it is
    // shown.
    refreshCanvas(w, state);
    setOwnWindowVisible(state.canvas.get(), true);

    // Every show() makes a fresh window of the store, so the one the scene knows
    // is picked up here rather than looked up again on every frame that draws
    // one.
    state.canvasWindow = effects->findWindow(state.canvas.get());
}

void ThumbnailBloomEffect::refreshCanvas(EffectWindow *w, BloomState &state)
{
    // What the store now holds was made with these, which is what lets a
    // thumbnail at rest be passed over entirely on the next frame; see
    // advanceAnimations(). Noted before the store is asked for, so that a
    // thumbnail that has none (a window held out of its bloom, whose frame came
    // down with it) is passed over just the same.
    state.outlineSerial = m_outlineSerial;
    state.bendOrigin = frameRect(w).center();
    state.canvasStale = false;

    if (!state.canvas) {
        return;
    }

    // How much of a thumbnail there is to frame, which is what the frame as a
    // whole fades with. The caption channel is exactly that measure already
    // (full at rest, gone on the way home and on a dive), and the hover empties
    // it while raising the highlight, so the two are added rather than the
    // larger of them taken: complementary channels crossing over would dip to a
    // half at the middle of every hover and blink the frame.
    const qreal strength = std::min(1.0, state.caption.current + state.highlight.current);

    // The hover does not raise the frame out of nothing; it thickens the one
    // that is there and turns it from the caption colour to the focus colour, so
    // a thumbnail is framed all the way through the animation and only ever
    // changes weight.
    const qreal blend = state.highlight.current;
    const qreal thickness = restOutlineWidth + (hoverOutlineWidth - restOutlineWidth) * blend;
    const QColor color = mixColors(m_restOutline, m_hoverOutline, blend);

    const QRectF rect = state.rect.current;
    if (rect.isEmpty() || strength <= outlineEpsilon) {
        state.canvas->setContent(QRectF(), {}, 0.0, color, 0.0, 0.0);
        return;
    }

    // The window is a store to paint frames into rather than the frame itself,
    // and it is kept at the largest rectangle the running trip will draw: the
    // grown one while the pointer is on the thumbnail, the real window while it
    // blooms out or travels home, and the resting one once there is no trip
    // left. So it is allocated when a trip starts instead of at every step of
    // one, and the line inside it is always painted at the size it is drawn on
    // the screen. The rectangle of this frame comes in as well, since a trip
    // that has just been restarted has not stepped yet.
    const QSizeF ends = state.timeline.done()
        ? state.rect.to.size()
        : QSizeF(std::max(state.rect.from.width(), state.rect.to.width()),
              std::max(state.rect.from.height(), state.rect.to.height()));

    // The store is moved onto the thumbnail every frame, and a window may only
    // sit at whole logical pixels, so it is snapped to the coarser grid that is
    // whole physical pixels as well; whatever fraction of a pixel is left over
    // is carried by the corners below, which are drawn at fractions of one
    // happily enough. That snap is also why the store is a step larger than the
    // rectangle it has to hold: it moves the corner everything is measured from.
    // The margin on every side is the room the line itself needs, being drawn
    // outside the thumbnail rather than on it.
    const qreal scale = deviceScale(w);
    const int step = deviceGridStep(scale);
    const int pad = static_cast<int>(std::ceil(paintMargin));
    const QPoint origin(floorToStep(rect.x() - pad, step), floorToStep(rect.y() - pad, step));
    const QSize size(
        static_cast<int>(std::ceil(std::max(ends.width(), rect.width()))) + 2 * pad + step,
        static_cast<int>(std::ceil(std::max(ends.height(), rect.height()))) + 2 * pad + step);
    const QRect geometry(origin, size);
    if (state.canvas->geometry() != geometry) {
        state.canvas->setGeometry(geometry);
    }

    // Where the thumbnail sits inside the store, down to the fraction of a pixel
    // the snap left over.
    const QRectF base(rect.topLeft() - QPointF(origin), rect.size());

    // Whole physical pixels, and never fewer than one: a pen a fraction of a
    // pixel wide covers the pixel at either edge of it by a fraction, which
    // comes out as two grey rows where one sharp row was asked for.
    const qreal pixel = 1.0 / scale;
    const qreal room = std::min(base.width(), base.height()) / 3.0;
    const qreal width = std::max(pixel, std::round(std::min(thickness, room) / pixel) * pixel);

    // Outside the rectangle rather than on it: the thumbnail is the picture, and
    // a line laid over it covers a strip of the very thing it draws attention
    // to. The band runs from the edge of the thumbnail outwards, and it is the
    // band that is put on the pixel grid rather than the thumbnail: its outer
    // boundary is snapped and its width is a whole number of pixels, so both of
    // its edges fall on pixel boundaries however the animation has left the
    // rectangle underneath. Whatever fraction of a pixel that leaves between the
    // two is covered by the bleed of the thumbnail. What the store is handed is
    // the inside of the band, the frame being filled outwards from there and
    // rounded on its outer corners alone, and those corners are bent by the very
    // map the pixels of the thumbnail are bent by.
    const QRectF band = roundToDevice(base.adjusted(-width, -width, width, width), scale);
    const QRectF inner = band.adjusted(width, width, -width, -width);
    const QTransform bend = stateBend(w, state, base);
    const std::array<QPointF, 4> corners = { bend.map(inner.topLeft()), bend.map(inner.topRight()),
        bend.map(inner.bottomRight()), bend.map(inner.bottomLeft()) };

    // The caption hangs from the same rectangle the line is drawn around, so it
    // travels with the picture; how opaque it is comes from its own channel.
    state.canvas->setContent(base, corners, width, color, strength, state.caption.current);
}

void ThumbnailBloomEffect::postPaintScreen()
{
    // The next frame of the animation is asked for here, over the ground this
    // step covered rather than over the whole screen; the pass it brings on
    // widens its damage by whatever is still owed to the screen it paints. It is
    // asked for whether or not the step was the last of the trip, since a step
    // taken in the pass of one screen has still to be drawn by the other, and
    // the frame that follows a step where nothing moved asks for nothing and
    // ends the chain.
    //
    // Nothing is asked for on behalf of the lifted thumbnails: they are drawn
    // again whenever a pass happens at all, and one that has come to rest needs
    // no pass of its own.
    if (!m_moved.isEmpty()) {
        effects->addRepaint(Region(m_moved));
    }

    Effect::postPaintScreen();
}

bool ThumbnailBloomEffect::isActive() const { return !m_states.empty(); }

bool ThumbnailBloomEffect::blocksDirectScanout() const { return !m_states.empty(); }

int ThumbnailBloomEffect::requestedEffectChainPosition() const { return 50; }

} // namespace ThumbnailBloom

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(
    ThumbnailBloom::ThumbnailBloomEffect, "metadata.json", return true;, return false;)

#include "thumbnailbloom.moc"
