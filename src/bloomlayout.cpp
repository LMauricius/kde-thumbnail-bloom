/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "bloomlayout.h"

#include <QRegion>

#include <cmath>
#include <limits>
#include <optional>
#include <qpoint.h>
#include <qsize.h>
#include <vector>
#include <ranges>

namespace ThumbnailBloom {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/*!Cuts out a rectangle mask from the area and stores the maximum remaining areas to output*/
void cutOut(const QRect &area, const QRect &mask, std::vector<QRect> &out)
{
    if (!mask.contains(area)) {
        if (!area.intersects(mask)) {
            out.push_back(area);
        } else {
            if (area.left() < mask.left())
                out.push_back({
                    QPoint { area.left(), area.top() },
                    QPoint { mask.left(), area.bottom() },
                });
            if (area.top() < mask.top())
                out.push_back({
                    QPoint { area.left(), area.top() },
                    QPoint { area.right(), mask.top() },
                });
            if (area.right() > mask.right())
                out.push_back({
                    QPoint { mask.right(), area.top() },
                    QPoint { area.right(), area.bottom() },
                });
            if (area.bottom() > mask.bottom())
                out.push_back({
                    QPoint { area.left(), mask.bottom() },
                    QPoint { area.right(), area.bottom() },
                });
        }
    }
}

static thread_local std::vector<QRect> rectsScratchpad;

/*!
 * A set of (possibly overlapping) rectangles describing a region of the screen.
 * Unlike QRegion, each rectangle spans the maximum area inside the region,
 * which makes ExtendedRegion more suitable for finding free areas.
 */
struct ExtendedRegion
{
    auto cbegin() const { return m_bands.cbegin(); }
    auto cend() const { return m_bands.cend(); }
    auto begin() const { return m_bands.cbegin(); }
    auto end() const { return m_bands.cend(); }

    ExtendedRegion() = default;
    ExtendedRegion(ExtendedRegion &&) = default;
    ExtendedRegion(const ExtendedRegion &) = default;
    ExtendedRegion &operator=(ExtendedRegion &&) = default;
    ExtendedRegion &operator=(const ExtendedRegion &) = default;

    ExtendedRegion(const QRect &r)
        : m_bands { r }
    { }

    ExtendedRegion &operator=(const QRect &r)
    {
        m_bands = { r };
        return *this;
    }

    ExtendedRegion &operator-=(const QRect &mask)
    {
        auto &oldBands = rectsScratchpad;

        // Swapped rather than copied: the bands are read once and thrown away,
        // and a screen with a few dozen windows on it carries several hundred of
        // them through a hundred subtractions. The scratchpad keeps the capacity
        // of whichever vector it last held, so after the first pass neither side
        // allocates again.
        oldBands.swap(m_bands);
        m_bands.clear();
        for (auto &b : oldBands) {
            cutOut(b, mask, m_bands);
        }
        return *this;
    }

    template <std::ranges::range M>
    ExtendedRegion &operator-=(const M &mask)
    {
        for (auto &m : mask) {
            *this -= m;
        }
        return *this;
    }

    ExtendedRegion operator-(const QRect &mask) &&
    {
        *this -= mask;
        return std::move(*this);
    }

    template <std::ranges::range M>
    ExtendedRegion operator-(const M &mask) &&
    {
        *this -= mask;
        return std::move(*this);
    }

private:
    std::vector<QRect> m_bands;
};

/*! Returns \a rect grown by \a margin on every side. */
static QRect grown(const QRectF &rect, int margin)
{
    return rect.toAlignedRect().adjusted(-margin, -margin, margin, margin);
}

/*! Returns whether \a rect lies completely inside \a region. */
static bool fitsInside(const ExtendedRegion &region, const QRect &rect)
{
    for (auto &r : region) {
        if (r.contains(rect))
            return true;
    }

    return false;
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

static qreal clamped(qreal value, qreal lower, qreal upper)
{
    return std::max(lower, std::min(value, upper));
}

constexpr qreal MIN_NOTICABLE_SIZE = 48.0;

/*!
 * Places a \a size sized rectangle as close to \a desiredCenter as the free
 * space in \a free allows. Returns nothing when \a size fits nowhere.
 * Otherwise returns a pair of the placement rect and cost
 */
static std::optional<std::pair<QRectF, qreal>> nearestPlacement(
    const QRectF &band, const QRectF &geometry, const QSizeF &initialSize)
{
    // Shrink (never grow) until the thumbnail fits the band, keeping the aspect ratio.
    QSizeF size = initialSize;

    if (size.width() > band.width()) {
        size *= band.width() / size.width();
    }
    if (size.height() > band.height()) {
        size *= band.height() / size.height();
    }

    // Each free band is a rectangle of its own; the closest position within a
    // band is the desired one clamped to the band's placeable range.
    const qreal hRange = band.width() - geometry.width();
    const qreal vRange = band.height() - geometry.height();
    const qreal hLocus
        = clamped(hRange > MIN_NOTICABLE_SIZE ? (geometry.x() - band.x()) / hRange
                                              : (geometry.center().x() - band.x()) / band.width(),
            0.0, 1.0);
    const qreal vLocus
        = clamped(vRange > MIN_NOTICABLE_SIZE ? (geometry.y() - band.y()) / vRange
                                              : (geometry.center().y() - band.y()) / band.height(),
            0.0, 1.0);

    const QRectF candidate {
        band.x() + hLocus * (band.width() - size.width()),
        band.y() + vLocus * (band.height() - size.height()),
        size.width(),
        size.height(),
    };

    const qreal distance = std::sqrt(distanceSquared(candidate.center(), geometry.center()));
    const qreal areaShrink
        = geometry.width() * geometry.height() / qreal(size.width() * size.height());

    return std::pair { candidate, distance * areaShrink };
}

/*!
 * Squared distance from \a rect's centre to the nearest corner of \a area.
 * Only ever compared against another such value, so the square root is spared.
 */
static qreal cornerDistanceSquared(const QRectF &area, const QRectF &rect)
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
 * Otherwise returns a pair of the placement rect and cost
 *
 * This is the packing counterpart of nearestFreeSlot(): it ignores where the
 * window actually is and pushes the rectangle into a corner, so that what is
 * left over stays one large block in the middle rather than a set of gaps too
 * small for anybody.
 */
static std::optional<std::pair<QRectF, qreal>> packedPlacement(
    const QRectF &band, const QRectF &geometry, const QSizeF &initialSize)
{
    QRectF best;
    qreal bestDistance = std::numeric_limits<qreal>::max();

    // Shrink (never grow) until the thumbnail fits the band, keeping the aspect ratio.
    QSizeF size = initialSize;

    if (size.width() > band.width()) {
        size *= band.width() / size.width();
    }
    if (size.height() > band.height()) {
        size *= band.height() / size.height();
    }

    // Every free band contributes its own four corners; the winner is the
    // corner that ends up nearest to a corner of the work area.

    const qreal lefts[] = { band.left(), band.right() - size.width() };
    const qreal tops[] = { band.top(), band.bottom() - size.height() };
    for (const qreal left : lefts) {
        for (const qreal top : tops) {
            const QRectF candidate(left, top, size.width(), size.height());

            const qreal distance = cornerDistanceSquared(geometry, candidate);
            if (distance < bestDistance) {
                best = candidate;
                bestDistance = distance;
            }
        }
    }

    return std::pair { best, bestDistance };
}

/*! A thumbnail rectangle together with the scale it was found at. */
struct SizedSlot
{
    QRectF rect;
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
static std::optional<SizedSlot> searchSlot(const ExtendedRegion &free, const QRectF &geometry,
    qreal startScale, const LayoutOptions &options, bool packed)
{
    // Shrink first, move second: try the starting size and only adjust it as needed to fit a spot
    const QSizeF initialSize = geometry.size() * startScale;

    // The smallest thumbnail either placement is allowed to come back with.
    // Both of them shrink the thumbnail to fit the band keeping its aspect
    // ratio, so a band shorter than this on either axis can only ever answer
    // with a scale below the minimum, which the test below the placement then
    // throws away. Asked here instead, it is two comparisons rather than a
    // placement worked out and discarded: cutting a work area up by a few dozen
    // windows leaves several hundred bands, most of them slivers no thumbnail
    // could ever sit in, and every one of them is offered to every thumbnail of
    // every pass.
    const qreal minWidth = geometry.width() * options.minScale;
    const qreal minHeight = geometry.height() * options.minScale;

    // Search for the best spot according to the cost.
    // Whether we're using the 'packed' layout affects both the placement method and cost function
    // After placement we will ensure that at least the minimum shrinking is performed.
    // If a spot would require shrinking past the min scale, skip it.
    std::optional<SizedSlot> bestSlot;
    qreal bestCost = std::numeric_limits<qreal>::max();
    for (const QRect &band : free) {
        if (band.width() < minWidth || band.height() < minHeight) {
            continue;
        }

        const std::optional<std::pair<QRectF, qreal>> placement = packed
            ? packedPlacement(band, geometry, initialSize)
            : nearestPlacement(band, geometry, initialSize);

        if (placement) {
            auto [candidate, cost] = placement.value();
            qreal scale = candidate.width() / geometry.width();

            if (cost >= bestCost || scale < options.minScale) {
                continue;
            }

            bestSlot = { candidate, scale };
            bestCost = cost;
        }
    }

    return bestSlot;
}

// ---------------------------------------------------------------------------
// Selection pass
// ---------------------------------------------------------------------------

/*!
 * Picks the windows that get a thumbnail, as a flag per entry of \a stack.
 *
 * Two rules, in order: enough of the window lies over a reserved window, so
 * that it is in the way of the window being worked in; or enough of it is
 * hidden by the windows that stay put and are not ignored outright (a backdrop
 * counts as hiding whether the settings exempt it or not). Both measure the same
 * LayoutOptions::minOccludedFraction of the window's own area, so a window
 * merely grazing another is left alone either way.
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
        // A backdrop hides what is behind it whatever the settings say: the
        // whole point of it is that those windows bloom out over it, and one
        // that took nothing with it would leave the screen exactly as covered
        // as it was.
        if (!stack[i].ignored || stack[i].backdrop) {
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
static constexpr qreal AVERAGE_BLEND = 0.33;

/*!
 * The part of the screen that is off limits to every thumbnail alike, before
 * any of them are handed out: the reserved windows, grown by the margin.
 *
 * Everything else a thumbnail has to avoid depends on where in the stack that
 * thumbnail sits, and is collected along the walk instead (see runPass()). The
 * window being worked in is the exception, since covering it is what the effect
 * is trying to undo; an ignored one claims nothing even then, exactly as it
 * claims nothing in the selection.
 */
static QRegion reservedRegion(const QList<LayoutWindow> &stack, const LayoutOptions &options)
{
    QRegion blocked;
    for (const LayoutWindow &window : stack) {
        if (window.reserved && !window.ignored) {
            blocked += grown(window.geometry.toAlignedRect(), options.margin);
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
 * Walking downwards is also what makes \a blocked mean "everything that stays
 * put above the window at hand": each window that is staying put joins the
 * region as the walk passes it, so it is in the way of the thumbnails of the
 * windows above it and of nobody else.
 *
 * \a startScale is the size every thumbnail is tried at first, \a packed picks
 * the slot strategy (see searchSlot()). \a averageScale, when given, receives
 * the mean of the scales the thumbnails ended up at, counting one that found no
 * room at all as LayoutOptions::minScale.
 */
static QList<Placement> runPass(const QList<LayoutWindow> &stack, std::vector<bool> &bloomed,
    const QRegion &seed, const QRect &area, const LayoutOptions &options, qreal startScale,
    bool packed, qreal *averageScale = nullptr)
{
    static thread_local ExtendedRegion uncovered;
    static thread_local ExtendedRegion free;
    uncovered = area;
    uncovered -= seed;
    free = area;
    free -= seed;

    QList<Placement> placements;
    qreal scaleSum = 0;
    int count = 0;

    // First build the region that is completely free of any relevant windows
    // When possible, thumbnails will be placed here,
    // rather than just over windows that are underneath them
    for (int i = 0; i < stack.size(); ++i) {
        const LayoutWindow &window = stack[i];
        const QRect geometry = window.geometry.toAlignedRect();

        if (!bloomed[i] && !window.backdrop) {
            free -= grown(geometry, options.margin);
        }
    }

    for (int i = stack.size() - 1; i >= 0; --i) {
        const LayoutWindow &window = stack[i];
        const QRect geometry = window.geometry.toAlignedRect();

        // A window that stays put is only ever in the way of the thumbnails of
        // the windows above it: a thumbnail belonging to a window below it is
        // painted under it anyway, so landing there takes nothing away that was
        // not already hidden. A backdrop is in nobody's way, which is what lets
        // a screen filled by one window still show thumbnails over it.
        if (!bloomed[i]) {
            if (!window.backdrop) {
                uncovered -= grown(geometry, options.margin);
            }
            continue;
        }
        ++count;

        std::optional<SizedSlot> slot
            = searchSlot(free, window.geometry, startScale, options, packed);

        // Since we couldn't put the thumbnail in the completely free area,
        // allow placement over the windows it would cover anyways
        if (!slot) {
            slot = searchSlot(uncovered, window.geometry, startScale, options, packed);
        }

        // Not even the smallest thumbnail fits: leave the window alone rather
        // than drop it somewhere it would be in the way. It stays where it is
        // and thus out of sight, under whatever covers it. It still counts
        // towards the average, at the smallest size, since the space it wanted
        // was not there.
        if (!slot) {
            scaleSum += options.minScale;
            auto g = grown(geometry, options.margin);
            free -= g;
            uncovered -= g;
            continue;
        }

        scaleSum += slot->scale;
        placements.append(Placement { window.id, QRectF(slot->rect) });
        auto g = grown(slot->rect, options.margin);
        free -= g;
        uncovered -= g;
    }

    if (averageScale) {
        *averageScale = count > 0 ? scaleSum / count : options.initialScale;
    }
    return placements;
}

QList<Placement> computeLayout(
    const QList<LayoutWindow> &stack, const QRectF &workArea, const LayoutOptions &options)
{
    // The margin is kept from whatever surrounds a thumbnail, and the edge of
    // the screen surrounds it just as much as a window does: the whole
    // placement therefore runs inside a work area shrunk by that same amount.
    const QRect area = workArea.toAlignedRect().adjusted(
        options.margin, options.margin, -options.margin, -options.margin);
    std::vector<bool> bloomed = selectBloomed(stack, options);
    const QRegion seed = reservedRegion(stack, options);

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
        options.initialScale + AVERAGE_BLEND * (averageScale - options.initialScale));

    // Placement pass: the real one, each thumbnail as close to its window as
    // the free space allows.
    return runPass(stack, bloomed, seed, area, options, startScale, false);
}

} // namespace ThumbnailBloom
