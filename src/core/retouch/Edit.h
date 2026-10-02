// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_RETOUCH_EDIT_H_
#define SCANTAILOR_RETOUCH_EDIT_H_

#include <BinaryImage.h>

#include <QColor>
#include <QPoint>
#include <QPolygonF>
#include <QRect>
#include <vector>

class QImage;
class QPainter;

namespace retouch {
/**
 * \brief One change painted over a source image.
 *
 * A filled rectangle, a brush stroke or a filled mask, in the pixel coordinates
 * of the image as stored in its file - before any rotation the project applies -
 * and with its colour already decided. Edits are kept as shapes rather than
 * painted straight into the image so that they can be undone, drawn over the
 * image on screen, and applied once, at save time, in the file's own pixel
 * format.
 */
class Edit {
 public:
  enum Kind { RECT, STROKE, MASK };

  /** \p rect in image pixels. */
  static Edit rect(const QRect& rect, const QColor& color);

  /**
   * A round-ended line through \p points, \p diameter pixels wide.
   *
   * The points are in continuous image coordinates, where pixel (x, y) spans
   * [x, x + 1) x [y, y + 1). A single point paints a disc.
   */
  static Edit stroke(const QPolygonF& points, double diameter, const QColor& color);

  /** The black pixels of \p mask, its top-left corner placed at \p origin. */
  static Edit mask(const imageproc::BinaryImage& mask, const QPoint& origin, const QColor& color);

  Kind kind() const { return m_kind; }

  const QColor& color() const { return m_color; }

  /**
   * \brief The same shape, painted with \p color.
   *
   * \param fromPaper Whether \p color is the paper's around the shape, to be
   *        taken again when the edit is carried over to another image.
   */
  Edit withColor(const QColor& color, bool fromPaper = false) const;

  bool colorFromPaper() const { return m_colorFromPaper; }

  /**
   * \brief The edit as it falls on an image \p sx times as wide and \p sy times as
   *        tall as the one it was made on.
   *
   * Errs on the side of covering more: a pixel touched at all by the scaled
   * shape is painted, so that nothing of what was painted over shows through
   * at the edges.
   */
  Edit scaled(double sx, double sy) const;

  /** \brief Every pixel the edit can touch. May extend past the image. */
  QRect boundingRect() const;

  /** \brief Black where the edit paints, over \p area, in image pixels. */
  imageproc::BinaryImage coverage(const QRect& area) const;

  const QRect& rect() const { return m_rect; }

  const QPolygonF& points() const { return m_points; }

  double diameter() const { return m_diameter; }

  const imageproc::BinaryImage& maskImage() const { return m_mask; }

  const QPoint& maskOrigin() const { return m_maskOrigin; }

 private:
  Edit(Kind kind, const QColor& color);

  Kind m_kind;
  QColor m_color;
  bool m_colorFromPaper;
  QRect m_rect;
  QPolygonF m_points;
  double m_diameter;
  imageproc::BinaryImage m_mask;
  QPoint m_maskOrigin;
};


/**
 * \brief Draws a brush stroke with whatever brush and pen \p painter is set up for.
 *
 * The one routine behind both the on-screen preview and the pixels written at
 * save time, so that what the operator sees is what ends up in the file. The
 * painter's colour is taken from \p color; antialiasing is the caller's choice.
 */
void paintStroke(QPainter& painter, const QPolygonF& points, double diameter, const QColor& color);

/**
 * \brief The colour \p requested becomes once written into \p image.
 *
 * Black-and-white and palette images can only take the colours they have: the
 * nearest one is used. Grayscale images take the grey of the same lightness.
 * Everything else takes the colour as it is.
 */
QColor effectiveColor(const QImage& image, const QColor& requested);

/** \brief Paints \p edits over \p image, in order, keeping its pixel format. */
void applyEdits(QImage& image, const std::vector<Edit>& edits);

/**
 * \brief \p edits made on one image, as they are to be painted on another copy of it.
 *
 * \p target is the other copy, \p sx and \p sy times the size of the first one.
 * Colours taken from the paper around an edit are taken again from \p target,
 * whose paper may not be the same shade: a cleaned copy, say, of a yellowed scan.
 */
std::vector<Edit> carryOver(const std::vector<Edit>& edits, double sx, double sy, const QImage& target, int paperRing);

/**
 * \brief The paper colour just outside the black pixels of \p coverage.
 *
 * Samples a band \p ringWidth pixels wide around the area \p coverage covers
 * when placed at \p origin, and takes the median of its lighter part, so that
 * ink brushing past the area does not tint the result.
 *
 * \return The colour, or an invalid colour if there is nothing to sample.
 */
QColor surroundingColor(const QImage& image,
                        const imageproc::BinaryImage& coverage,
                        const QPoint& origin,
                        int ringWidth);

/**
 * \brief The colour at \p center, as the median of a small square around it.
 *
 * A single pixel of a scan is noise: paper grain, a speck of dust. The median
 * of its neighbourhood is the colour the operator means.
 */
QColor sampleColor(const QImage& image, const QPoint& center, int radius);
}  // namespace retouch

#endif  // SCANTAILOR_RETOUCH_EDIT_H_
