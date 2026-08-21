/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QPointF>
#include <QRectF>
#include <QVector2D>

#include <array>

namespace ThumbnailBloom
{

/*!
 * The four corners of a bent thumbnail, clockwise from the top left.
 *
 * The order is the one KWin's WindowQuad expects, so the corners can be handed
 * to a quad-to-quad transform without being reshuffled first.
 */
using BendQuad = std::array<QPointF, 4>;

/*!
 * Returns the corners of \a rect turned \a angle degrees towards \a direction.
 *
 * \a direction is a screen-space vector (y downwards) pointing at the side that
 * turns away from the viewer: the rectangle pivots about the in-plane axis
 * perpendicular to it, and the perspective divide that follows makes the far
 * side the short one. Its length is ignored, only where it points matters.
 *
 * Each thumbnail is bent on its own, in its own little perspective, rather than
 * placed in one shared 3D scene: the result is fitted back into \a rect, so a
 * bent thumbnail keeps exactly the footprint the layout gave it and can never
 * reach into a neighbour's. An \a angle of zero (or a null \a direction) gives
 * the corners of \a rect unchanged.
 */
BendQuad bendQuad(const QRectF &rect, qreal angle, const QVector2D &direction);

} // namespace ThumbnailBloom
