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
 * A window is turned into a thumbnail when something above it covers a part of
 * it. The thumbnail is shrunk to LayoutOptions::initialScale and put in the
 * free space nearest to where the window sits; if nothing fits, it keeps
 * shrinking down to LayoutOptions::minScale. The area of a
 * LayoutWindow::reserved window is kept free whatever its place in the stack.
 *
 * Returns one Placement per bloomed window; windows that stay untouched and
 * windows the smallest thumbnail still finds no room for are absent from the
 * result.
 */
QList<Placement> computeLayout(const QList<LayoutWindow> &stack, const QRectF &workArea, const LayoutOptions &options);

} // namespace ThumbnailBloom
