/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thumbnailbend.h"

#include <QPolygonF>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace ThumbnailBloom {

/*!
 * How far the eye sits from the thumbnail, in multiples of its longer side.
 *
 * It is what decides how strong the foreshortening is at a given angle: the
 * smaller it gets, the more the near side flares out over the far one. Kept
 * proportional to the thumbnail so that every thumbnail bends alike whatever
 * its size.
 */
constexpr qreal viewDistance = 2.0;

/*! Returns the dot product of \a a and \a b. */
static qreal dot(const QPointF &a, const QPointF &b) { return a.x() * b.x() + a.y() * b.y(); }

/*! Returns the corners of \a rect in quad order: clockwise from the top left. */
static BendQuad cornersOf(const QRectF &rect)
{
    return { rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft() };
}

BendQuad bendQuad(const QRectF &rect, qreal angle, const QVector2D &direction)
{
    const BendQuad flat = cornersOf(rect);

    const qreal length = std::hypot(direction.x(), direction.y());
    if (rect.isEmpty() || qFuzzyIsNull(angle) || qFuzzyIsNull(length)) {
        return flat;
    }

    // The turn is described in the frame of the direction: `along` is the way the
    // rectangle tilts, `across` the axis it pivots about, which keeps its length.
    const QPointF along(direction.x() / length, direction.y() / length);
    const QPointF across(-along.y(), along.x());

    const qreal radians = qDegreesToRadians(angle);
    const qreal cosine = std::cos(radians);
    const qreal sine = std::sin(radians);
    const QPointF centre = rect.center();
    const qreal distance = viewDistance * std::max(rect.width(), rect.height());

    BendQuad bent;
    for (size_t i = 0; i < flat.size(); ++i) {
        const QPointF local = flat[i] - centre;
        const qreal forward = dot(local, along);
        const qreal sideways = dot(local, across);

        // The turn itself: the half towards `direction` swings away from the eye
        // and the other half towards it, so the depth is centred on the pivot.
        // The distance is always larger than half the diagonal, so the eye can
        // never end up inside the thumbnail and the divide below stays positive.
        const qreal depth = forward * sine;
        const qreal shrink = distance / (distance + depth);

        bent[i] = centre + (forward * cosine * shrink) * along + (sideways * shrink) * across;
    }

    // Fitted back into the rectangle it came from. The bend narrows the shape and
    // shifts it towards the far side, and letting that through would both shrink
    // the thumbnail as the angle rises and let it drift over its neighbours.
    QRectF bounds(bent[0], bent[0]);
    for (const QPointF &corner : bent) {
        bounds.setLeft(std::min(bounds.left(), corner.x()));
        bounds.setRight(std::max(bounds.right(), corner.x()));
        bounds.setTop(std::min(bounds.top(), corner.y()));
        bounds.setBottom(std::max(bounds.bottom(), corner.y()));
    }
    if (bounds.isEmpty()) {
        return flat;
    }

    const qreal scaleX = rect.width() / bounds.width();
    const qreal scaleY = rect.height() / bounds.height();
    for (QPointF &corner : bent) {
        corner = QPointF(rect.x() + (corner.x() - bounds.x()) * scaleX,
            rect.y() + (corner.y() - bounds.y()) * scaleY);
    }

    return bent;
}

QTransform bendTransform(const QRectF &rect, qreal angle, const QVector2D &direction)
{
    if (rect.isEmpty()) {
        return QTransform();
    }

    const BendQuad bent = bendQuad(rect, angle, direction);
    const BendQuad corners = cornersOf(rect);
    const QPolygonF flat({ corners[0], corners[1], corners[2], corners[3] });

    QTransform transform;
    if (!QTransform::quadToQuad(
            flat, QPolygonF({ bent[0], bent[1], bent[2], bent[3] }), transform)) {
        return QTransform();
    }
    return transform;
}

} // namespace ThumbnailBloom
