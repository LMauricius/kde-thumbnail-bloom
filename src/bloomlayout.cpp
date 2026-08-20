/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "bloomlayout.h"

#include <QRegion>

#include <cmath>
#include <optional>

namespace ThumbnailBloom
{

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
static std::optional<QRect> nearestFreeSlot(const QRegion &free, const QSize &size, const QPointF &desiredCenter)
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
        const QRect candidate(clamped(desiredTopLeft.x(), band.left(), band.right() + 1 - size.width()),
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
 * Last resort placement: keeps the thumbnail at \a size next to its window and
 * only makes sure it stays on \a workArea.
 */
static QRect clampedToWorkArea(const QSize &size, const QPointF &desiredCenter, const QRect &workArea)
{
    QRect rect(QPoint(0, 0), size);
    rect.moveCenter(QPoint(std::lround(desiredCenter.x()), std::lround(desiredCenter.y())));
    rect.moveLeft(clamped(rect.left(), workArea.left(), std::max(workArea.left(), workArea.right() + 1 - size.width())));
    rect.moveTop(clamped(rect.top(), workArea.top(), std::max(workArea.top(), workArea.bottom() + 1 - size.height())));
    return rect;
}

// ---------------------------------------------------------------------------
// Layout pass
// ---------------------------------------------------------------------------

QList<Placement> computeLayout(const QList<LayoutWindow> &stack, const QRectF &workArea, const LayoutOptions &options)
{
    const QRect area = workArea.toAlignedRect();
    QList<Placement> placements;

    // Everything that a window further down may not be placed on: the windows
    // above it plus the thumbnails already handed out.
    QRegion occupied;
    QRegion blocked;

    // Walk from the top of the stack downwards, so that by the time a window is
    // looked at, everything covering it has its final rectangle already.
    for (int i = stack.size() - 1; i >= 0; --i) {
        const LayoutWindow &window = stack[i];
        const QRect geometry = window.geometry.toAlignedRect();

        const bool overlapped = occupied.intersects(geometry);
        if (!window.eligible || !overlapped) {
            occupied += geometry;
            blocked += grown(geometry, options.margin);
            continue;
        }

        // Shrink first, move second: try the configured thumbnail size and only
        // shrink further when that size finds no free spot.
        const QRegion free = QRegion(area).subtracted(blocked);
        const QPointF desiredCenter = window.geometry.center();
        std::optional<QRect> slot;

        for (qreal scale = options.initialScale; scale >= options.minScale - 0.001; scale -= options.scaleStep) {
            const QSize size(std::max(1, int(std::lround(window.geometry.width() * scale))),
                             std::max(1, int(std::lround(window.geometry.height() * scale))));
            slot = nearestFreeSlot(free, size, desiredCenter);
            if (slot) {
                break;
            }
        }

        // Nothing fits anywhere: show the window at the smallest allowed size.
        if (!slot) {
            const QSize size(std::max(1, int(std::lround(window.geometry.width() * options.minScale))),
                             std::max(1, int(std::lround(window.geometry.height() * options.minScale))));
            slot = clampedToWorkArea(size, desiredCenter, area);
        }

        placements.append(Placement{window.id, QRectF(*slot)});
        occupied += *slot;
        blocked += grown(*slot, options.margin);
    }

    return placements;
}

} // namespace ThumbnailBloom
