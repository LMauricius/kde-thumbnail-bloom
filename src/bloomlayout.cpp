/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "bloomlayout.h"

#include <QRegion>

#include <cmath>
#include <optional>
#include <vector>

namespace ThumbnailBloom {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/*! Returns \a rect grown by \a margin on every side. */
static QRect grown(const QRect &rect, int margin)
{
    return rect.adjusted(-margin, -margin, margin, margin);
}

/*! Returns whether \a rect lies completely inside \a region. */
static bool fitsInside(const QRegion &region, const QRect &rect)
{
    return QRegion(rect).subtracted(region).isEmpty();
}

/*!
 * Returns whether \a region covers at least \a fraction of \a rect. An empty
 * rectangle is never covered enough, whatever the fraction.
 */
static bool coversEnough(const QRegion &region, const QRect &rect, qreal fraction)
{
    qreal covered = 0;
    for (const QRect &part : region.intersected(rect)) {
        covered += qreal(part.width()) * part.height();
    }
    return covered > 0 && covered >= qreal(rect.width()) * rect.height() * fraction;
}

/*! Squared distance between two points; good enough for comparing candidates. */
static qreal distanceSquared(const QPointF &a, const QPointF &b)
{
    const qreal dx = a.x() - b.x();
    const qreal dy = a.y() - b.y();
    return dx * dx + dy * dy;
}

/*! Clamps \a value into [\a lower, \a upper], tolerating an empty range. */
static int clamped(int value, int lower, int upper)
{
    return std::max(lower, std::min(value, upper));
}

/*!
 * Places a \a size sized rectangle as close to \a desiredCenter as the free
 * space in \a free allows. Returns nothing when \a size fits nowhere.
 */
static std::optional<QRect> nearestFreeSlot(
    const QRegion &free, const QSize &size, const QPointF &desiredCenter)
{
    std::optional<QRect> best;
    qreal bestDistance = 0;

    // Each free band is a rectangle of its own; the closest position within a
    // band is the desired one clamped to the band's placeable range.
    for (const QRect &band : free) {
        if (band.width() < size.width() || band.height() < size.height()) {
            continue;
        }

        const QPoint desiredTopLeft(std::lround(desiredCenter.x() - size.width() / 2.0),
            std::lround(desiredCenter.y() - size.height() / 2.0));
        const QRect candidate(
            clamped(desiredTopLeft.x(), band.left(), band.right() + 1 - size.width()),
            clamped(desiredTopLeft.y(), band.top(), band.bottom() + 1 - size.height()),
            size.width(), size.height());

        // Bands are only single rows of the region, so a candidate that sticks
        // out vertically into a neighbouring band still has to be checked.
        if (!fitsInside(free, candidate)) {
            continue;
        }

        const qreal distance = distanceSquared(candidate.center(), desiredCenter);
        if (!best || distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

/*!
 * Squared distance from \a rect's centre to the nearest corner of \a area.
 * Only ever compared against another such value, so the square root is spared.
 */
static qreal cornerDistanceSquared(const QRect &area, const QRect &rect)
{
    const QPointF center = rect.center();
    const QPointF corners[]
        = { area.topLeft(), area.topRight(), area.bottomLeft(), area.bottomRight() };

    qreal best = distanceSquared(center, corners[0]);
    for (const QPointF &corner : corners) {
        best = std::min(best, distanceSquared(center, corner));
    }
    return best;
}

/*!
 * Places a \a size sized rectangle in whichever corner of the free space in
 * \a free lies closest to a corner of \a area. Returns nothing when \a size
 * fits nowhere.
 *
 * This is the packing counterpart of nearestFreeSlot(): it ignores where the
 * window actually is and pushes the rectangle into a corner, so that what is
 * left over stays one large block in the middle rather than a set of gaps too
 * small for anybody.
 */
static std::optional<QRect> packedFreeSlot(
    const QRegion &free, const QSize &size, const QRect &area)
{
    std::optional<QRect> best;
    qreal bestDistance = 0;

    // Every free band contributes its own four corners; the winner is the
    // corner that ends up nearest to a corner of the work area.
    for (const QRect &band : free) {
        if (band.width() < size.width() || band.height() < size.height()) {
            continue;
        }

        const int lefts[] = { band.left(), band.right() + 1 - size.width() };
        const int tops[] = { band.top(), band.bottom() + 1 - size.height() };
        for (const int left : lefts) {
            for (const int top : tops) {
                const QRect candidate(left, top, size.width(), size.height());

                // Bands are only single rows of the region, so a candidate that
                // sticks out vertically into a neighbouring band still has to be
                // checked.
                if (!fitsInside(free, candidate)) {
                    continue;
                }

                const qreal distance = cornerDistanceSquared(area, candidate);
                if (!best || distance < bestDistance) {
                    best = candidate;
                    bestDistance = distance;
                }
            }
        }
    }

    return best;
}

/*! A thumbnail rectangle together with the scale it was found at. */
struct SizedSlot
{
    QRect rect;
    qreal scale = 0;
};

/*!
 * Shrinks a thumbnail of \a geometry from \a startScale down to
 * LayoutOptions::minScale until one of the sizes finds a slot in \a free.
 *
 * \a packed picks the strategy: packedFreeSlot() into a corner of \a area, or
 * nearestFreeSlot() to where the window sits. Returns nothing when not even
 * the smallest size fits anywhere.
 */
static std::optional<SizedSlot> searchSlot(const QRegion &free, const QRectF &geometry,
    const QRect &area, qreal startScale, const LayoutOptions &options, bool packed)
{
    // Shrink first, move second: try the starting size and only shrink further
    // when that size finds no free spot. The loop always ends on minScale
    // rather than on whatever the steps happen to land on, since startScale is
    // no multiple of scaleStep and the smallest size must never be skipped.
    qreal scale = std::max(options.minScale, startScale);
    while (true) {
        const QSize size(std::max(1, int(std::lround(geometry.width() * scale))),
            std::max(1, int(std::lround(geometry.height() * scale))));
        const std::optional<QRect> slot = packed
            ? packedFreeSlot(free, size, area)
            : nearestFreeSlot(free, size, geometry.center());
        if (slot) {
            return SizedSlot { *slot, scale };
        }

        if (scale <= options.minScale + 0.001) {
            return {};
        }
        scale = std::max(options.minScale, scale - options.scaleStep);
    }
}

// ---------------------------------------------------------------------------
// Selection pass
// ---------------------------------------------------------------------------

/*!
 * Picks the windows that get a thumbnail, as a flag per entry of \a stack.
 *
 * Two rules, in order: enough of the window lies over a reserved window, so
 * that it is in the way of the window being worked in; or enough of it is
 * hidden by the windows that stay put and are not ignored outright. Both
 * measure the same LayoutOptions::minOccludedFraction of the window's own area,
 * so a window merely grazing another is left alone either way.
 */
static std::vector<bool> selectBloomed(
    const QList<LayoutWindow> &stack, const LayoutOptions &options)
{
    std::vector<bool> bloomed(stack.size(), false);

    // Rule one: sharing space with a reserved window, whatever the stacking
    // says. A window above the active one hides just as much of it as one below.
    // An ignored window claims nothing even while it is the active one: the
    // settings keep it out of the effect, and clearing space around it is part
    // of the effect.
    QRegion reserved;
    for (const LayoutWindow &window : stack) {
        if (window.reserved && !window.ignored) {
            reserved += window.geometry.toAlignedRect();
        }
    }
    for (int i = 0; i < stack.size(); ++i) {
        if (stack[i].eligible && !stack[i].reserved
            && coversEnough(
                reserved, stack[i].geometry.toAlignedRect(), options.minOccludedFraction)) {
            bloomed[i] = true;
        }
    }

    // Rule two: walk from the top down keeping the windows that stay where they
    // are, and bloom whatever they hide enough of. Windows picked by rule one
    // are leaving, so they hide nothing and are left out of the running region;
    // that is what stops one thumbnail from dragging the whole stack under it
    // along with it. A window only a sliver of which is covered is still usable
    // where it is, hence the threshold rather than a plain intersection test.
    //
    // Ignored windows are left out of the region as well: the settings keep them
    // out of the effect altogether, so one must not push the windows under it
    // aside either. That is narrower than plain ineligibility, since the window
    // being moved or resized is no candidate for a thumbnail yet still hides
    // whatever it is dragged over.
    QRegion cover;
    for (int i = stack.size() - 1; i >= 0; --i) {
        if (bloomed[i]) {
            continue;
        }
        const QRect geometry = stack[i].geometry.toAlignedRect();
        if (stack[i].eligible && coversEnough(cover, geometry, options.minOccludedFraction)) {
            bloomed[i] = true;
            continue;
        }
        if (!stack[i].ignored) {
            cover += geometry;
        }
    }

    return bloomed;
}

// ---------------------------------------------------------------------------
// Layout pass
// ---------------------------------------------------------------------------

/*!
 * How far the placement pass moves its starting size away from the configured
 * thumbnail size, towards the average size the sizing pass could actually
 * afford. Zero would keep the old greedy behaviour; one would hand every
 * thumbnail the average outright.
 */
static constexpr qreal AverageBlend = 0.33;

/*!
 * Everything a thumbnail may not be placed on before any of them are handed
 * out: every window of \a stack that is staying put, grown by the margin.
 *
 * Stacking has no say here, so the whole set is collected up front: a thumbnail
 * dropped on a window below it in the stack would still be covering that window.
 */
static QRegion blockedSeed(const QList<LayoutWindow> &stack, const std::vector<bool> &bloomed,
    const LayoutOptions &options)
{
    QRegion blocked;
    for (int i = 0; i < stack.size(); ++i) {
        if (!bloomed[i]) {
            blocked += grown(stack[i].geometry.toAlignedRect(), options.margin);
        }
    }
    return blocked;
}

/*!
 * Hands every bloomed window of \a stack a rectangle, walking from the top of
 * the stack downwards so that the thumbnails nearer the top get the pick of the
 * free space and each one is placed against the final rectangles of those above
 * it.
 *
 * \a startScale is the size every thumbnail is tried at first, \a packed picks
 * the slot strategy (see searchSlot()). \a averageScale, when given, receives
 * the mean of the scales the thumbnails ended up at, counting one that found no
 * room at all as LayoutOptions::minScale.
 */
static QList<Placement> runPass(const QList<LayoutWindow> &stack, const std::vector<bool> &bloomed,
    const QRegion &seed, const QRect &area, const LayoutOptions &options, qreal startScale,
    bool packed, qreal *averageScale = nullptr)
{
    QRegion blocked = seed;
    QList<Placement> placements;
    qreal scaleSum = 0;
    int count = 0;

    for (int i = stack.size() - 1; i >= 0; --i) {
        if (!bloomed[i]) {
            continue;
        }
        const LayoutWindow &window = stack[i];
        const QRect geometry = window.geometry.toAlignedRect();
        ++count;

        const QRegion free = QRegion(area).subtracted(blocked);
        const std::optional<SizedSlot> slot
            = searchSlot(free, window.geometry, area, startScale, options, packed);

        // Not even the smallest thumbnail fits: leave the window alone rather
        // than drop it somewhere it would be in the way. It stays where it is
        // and thus out of sight, under whatever covers it. It still counts
        // towards the average, at the smallest size, since the space it wanted
        // was not there.
        if (!slot) {
            scaleSum += options.minScale;
            blocked += grown(geometry, options.margin);
            continue;
        }

        scaleSum += slot->scale;
        placements.append(Placement { window.id, QRectF(slot->rect) });
        blocked += grown(slot->rect, options.margin);
    }

    if (averageScale) {
        *averageScale = count > 0 ? scaleSum / count : options.initialScale;
    }
    return placements;
}

QList<Placement> computeLayout(
    const QList<LayoutWindow> &stack, const QRectF &workArea, const LayoutOptions &options)
{
    const QRect area = workArea.toAlignedRect();
    const std::vector<bool> bloomed = selectBloomed(stack, options);
    const QRegion seed = blockedSeed(stack, bloomed, options);

    // Sizing pass: the same walk, but packing every thumbnail into a corner
    // instead of putting it where it looks best. That leaves the free space in
    // one block rather than in scraps, so what the later thumbnails have to
    // settle for is a fair measure of how much room this screen really has. The
    // rectangles are thrown away; only the average size survives.
    qreal averageScale = options.initialScale;
    runPass(stack, bloomed, seed, area, options, options.initialScale, true, &averageScale);

    // Every thumbnail now starts a third of the way from the configured size
    // towards that average, so a crowded screen asks for less up front and the
    // thumbnails further down the stack are no longer left with the scraps. A
    // screen where everything fitted measures the configured size as its
    // average and is laid out exactly as before.
    const qreal startScale = std::max(options.minScale,
        options.initialScale + AverageBlend * (averageScale - options.initialScale));

    // Placement pass: the real one, each thumbnail as close to its window as
    // the free space allows.
    return runPass(stack, bloomed, seed, area, options, startScale, false);
}

} // namespace ThumbnailBloom
