/*
    SPDX-FileCopyrightText: 2026 Mauricio S.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QList>
#include <QRectF>

namespace ThumbnailBloom
{

/*!
 * One window as seen by the layout pass. Deliberately free of any KWin type so
 * that the placement maths can be exercised on its own.
 */
struct LayoutWindow
{
    void *id = nullptr; //!< opaque handle, handed back in the resulting Placement
    QRectF geometry; //!< where the window currently is on screen
    bool eligible = false; //!< whether this window may be turned into a thumbnail
    bool reserved = false; //!< whether no thumbnail may ever be placed over this window
    bool ignored = false; //!< whether the settings exempt this window from the effect entirely; overrides both flags above
};

/*!
 * Tunables of the placement search, filled from the effect's settings.
 */
struct LayoutOptions
{
    qreal initialScale = 0.5; //!< first size a thumbnail is tried at
    qreal minScale = 0.15; //!< smallest size the search may fall back to
    qreal scaleStep = 0.05; //!< how much smaller each unsuccessful attempt gets
    int margin = 8; //!< gap kept between a thumbnail and its surroundings
    qreal minOccludedFraction = 0.08; //!< how much of a window must be covered before it blooms
};

/*!
 * Where one window ends up once it has been shrunk and pushed aside.
 */
struct Placement
{
    void *id = nullptr;
    QRectF rect;
};

/*!
 * Computes thumbnail rectangles for the eligible windows of a single screen.
 *
 * \a stack holds the windows of that screen in stacking order, bottom-most
 * first. \a workArea is the area thumbnails may use.
 *
 * Selection comes first, placement second, so that the set of thumbnails does
 * not depend on where the previous ones happened to land. A window is turned
 * into a thumbnail when LayoutOptions::minOccludedFraction of its area lies
 * over a LayoutWindow::reserved window (wherever the two sit in the stack), or
 * is hidden by the windows above it that are staying put. Windows that are themselves becoming thumbnails cover
 * nothing, so one thumbnail does not pull the windows under it along; neither do
 * the LayoutWindow::ignored ones, which the settings keep out of the effect
 * altogether rather than letting them push others aside.
 *
 * The thumbnail is then shrunk to LayoutOptions::initialScale and put in the
 * free space nearest to where the window sits; if nothing fits, it keeps
 * shrinking down to LayoutOptions::minScale. Every window that is staying put
 * is kept clear, wherever it sits in the stack, so a thumbnail never lands on
 * a window that was visible without it.
 *
 * Returns one Placement per bloomed window; windows that stay untouched and
 * windows the smallest thumbnail still finds no room for are absent from the
 * result.
 */
QList<Placement> computeLayout(const QList<LayoutWindow> &stack, const QRectF &workArea, const LayoutOptions &options);

} // namespace ThumbnailBloom
