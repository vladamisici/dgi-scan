// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_RETOUCH_OUTPUTLAYER_H_
#define SCANTAILOR_RETOUCH_OUTPUTLAYER_H_

#include <QSize>
#include <QTransform>
#include <vector>

#include "Edit.h"

class QDomDocument;
class QDomElement;
class QImage;
class QString;

namespace retouch {
/**
 * \brief The retouching of a page's output, kept with the project.
 *
 * Like the fill zones, it is part of the page's settings rather than of a file:
 * each time the output is made it is painted over it again, so making the
 * output anew - another threshold, other margins - does not lose it.
 *
 * The edits are in the pixels of the output they were painted on, kept with
 * the transform that output was made with, from the source image's pixels to
 * its own. On an output made the same way they are painted as they are; on
 * one whose geometry has changed since - new margins, a corrected skew - they
 * are moved with the page first. A dewarped output is not an affine image of
 * its source, so on one of those they are only painted while nothing about its
 * geometry has changed.
 */
class OutputLayer {
 public:
  OutputLayer() = default;

  explicit OutputLayer(const QDomElement& el);

  QDomElement toXml(QDomDocument& doc, const QString& name) const;

  bool isEmpty() const { return m_edits.empty(); }

  const std::vector<Edit>& edits() const { return m_edits; }

  /**
   * \brief This layer with \p edits added, painted on an output made with
   *        \p originalToOutput, \p outputSize pixels.
   *
   * The edits already here are moved onto that output first, if it was made
   * differently. \p dropped, if given, is set to whether some could not be and
   * were left out: they were made on a dewarped output that has changed since.
   */
  OutputLayer adding(const std::vector<Edit>& edits,
                     const QTransform& originalToOutput,
                     const QSize& outputSize,
                     bool dewarped,
                     bool* dropped = nullptr) const;

  /**
   * \brief The edits as they fall on an output made with \p originalToOutput.
   *
   * \return false, with \p edits untouched, if they cannot be placed on it.
   */
  bool placed(const QTransform& originalToOutput,
              const QSize& outputSize,
              bool dewarped,
              std::vector<Edit>* edits) const;

  /**
   * \brief Paints the edits over \p output, made with \p originalToOutput.
   *
   * Colours taken from the paper are taken again, from \p output's own paper
   * within \p paperRing pixels of each edit: the output may be made in other
   * colours now.
   *
   * \return false, painting nothing, if the edits cannot be placed on it.
   */
  bool applyTo(QImage& output, const QTransform& originalToOutput, bool dewarped, int paperRing) const;

 private:
  std::vector<Edit> m_edits;
  QTransform m_originalToOutput;
  QSize m_outputSize;
  bool m_dewarped = false;
};


/**
 * \brief \p edits moved by \p transform.
 *
 * Each becomes the mask of the pixels its moved shape touches at all, so that
 * nothing of what it painted over shows at the edges.
 */
std::vector<Edit> transformed(const std::vector<Edit>& edits, const QTransform& transform);
}  // namespace retouch

#endif  // SCANTAILOR_RETOUCH_OUTPUTLAYER_H_
