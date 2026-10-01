// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_RETOUCH_SELECTION_H_
#define SCANTAILOR_RETOUCH_SELECTION_H_

#include <BinaryImage.h>

#include <QColor>
#include <QPoint>
#include <QRect>

class QImage;

namespace retouch {
/**
 * \brief Some pixels of a source image: the black pixels of a mask placed at an origin.
 *
 * The mask is kept trimmed to the box around its black pixels, so that a
 * selection costs memory in proportion to what it covers rather than to the
 * page.
 */
class Selection {
 public:
  Selection() = default;

  Selection(const imageproc::BinaryImage& mask, const QPoint& origin);

  bool isNull() const { return m_mask.isNull(); }

  const imageproc::BinaryImage& mask() const { return m_mask; }

  const QPoint& origin() const { return m_origin; }

  /** \brief The box around the selected pixels, in image pixels. */
  QRect rect() const;

  Selection united(const Selection& other) const;

  Selection subtracted(const Selection& other) const;

 private:
  void trim();

  imageproc::BinaryImage m_mask;
  QPoint m_origin;
};


struct ObjectSelectionParams {
  /**
   * How far a pixel's colour must be from the paper's, 0-255 in its most
   * different channel, for the pixel to count as ink.
   */
  int tolerance = 60;
  /** Ink no further than this many pixels from the object joins it: the letters of a stamp, its frame. */
  int grouping = 12;
  /** How many pixels past the ink the selection reaches, to take the soft edge of the ink along. */
  int margin = 2;
  /** A click on paper no further than this many pixels from ink takes that ink. */
  int seedSearchRadius = 8;
};


/**
 * \brief Selects the object under \p seed: a stamp, a blot, a smudge, a line of text.
 *
 * Ink is whatever differs from the colour of the paper around it by more than
 * the tolerance. The object is the ink connected to the clicked point, where ink
 * within the grouping distance counts as connected - so that one click takes all
 * the letters of a stamp together with the frame around them. Paper inside the
 * object is not selected: filling the selection leaves the grain of the paper
 * between the letters as it was.
 *
 * \return The selection, or a null one if there is no ink at or near \p seed.
 */
Selection selectObject(const QImage& image, const QPoint& seed, const ObjectSelectionParams& params);

/** \brief The colour of the paper in \p area of \p image: the median of its lighter half. */
QColor estimatePaperColor(const QImage& image, const QRect& area);
}  // namespace retouch

#endif  // SCANTAILOR_RETOUCH_SELECTION_H_
