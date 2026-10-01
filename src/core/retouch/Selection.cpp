// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "Selection.h"

#include <Connectivity.h>
#include <Morphology.h>
#include <RasterOp.h>
#include <SeedFill.h>

#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "Diagnostics.h"

using namespace imageproc;

namespace retouch {
namespace {
/**
 * How far the first search window reaches from the click, in pixels. A stamp
 * on a 600 dpi scan is some 1000-1500 pixels across. An object that runs into
 * the edge of the window gets the search repeated over twice the reach.
 */
const int INITIAL_REACH = 1024;

/** Rows converted to 32 bits at a time, so that a whole page is never held in that form. */
const int BAND_HEIGHT = 256;

/** Black where a pixel of \p area of \p image differs from \p paper by more than \p tolerance. */
BinaryImage inkMask(const QImage& image, const QRect& area, const QColor& paper, const int tolerance) {
  BinaryImage ink(area.size(), WHITE);
  uint32_t* const inkData = ink.data();
  const int wpl = ink.wordsPerLine();
  const int paperRed = paper.red();
  const int paperGreen = paper.green();
  const int paperBlue = paper.blue();
  const int width = area.width();

  for (int bandTop = 0; bandTop < area.height(); bandTop += BAND_HEIGHT) {
    const int bandHeight = std::min(BAND_HEIGHT, area.height() - bandTop);
    const QImage band(
        image.copy(area.x(), area.y() + bandTop, width, bandHeight).convertToFormat(QImage::Format_RGB32));
    for (int y = 0; y < bandHeight; ++y) {
      const auto* src = reinterpret_cast<const uint32_t*>(band.constScanLine(y));
      uint32_t* dst = inkData + ptrdiff_t(bandTop + y) * wpl;
      for (int x = 0; x < width; ++x) {
        const uint32_t px = src[x];
        // The channel that differs most, so that a red stamp on cream paper
        // counts as much as a black one: averaging the channels would dilute it.
        const int diff = std::max({std::abs(int((px >> 16) & 0xff) - paperRed),
                                   std::abs(int((px >> 8) & 0xff) - paperGreen), std::abs(int(px & 0xff) - paperBlue)});
        if (diff > tolerance) {
          dst[x >> 5] |= uint32_t(0x80000000) >> (x & 31);
        }
      }
    }
  }
  return ink;
}

/** The ink pixel nearest to \p point within \p radius, or (-1, -1) if there is none. */
QPoint nearestInk(const BinaryImage& ink, const QPoint& point, const int radius) {
  if (ink.rect().contains(point) && (ink.getPixel(point.x(), point.y()) == BLACK)) {
    return point;
  }
  QPoint best(-1, -1);
  int bestSqDist = radius * radius + 1;
  const QRect area(
      QRect(point - QPoint(radius, radius), QSize(2 * radius + 1, 2 * radius + 1)).intersected(ink.rect()));
  for (int y = area.top(); y <= area.bottom(); ++y) {
    for (int x = area.left(); x <= area.right(); ++x) {
      const int dx = x - point.x();
      const int dy = y - point.y();
      const int sqDist = dx * dx + dy * dy;
      if ((sqDist < bestSqDist) && (ink.getPixel(x, y) == BLACK)) {
        best = QPoint(x, y);
        bestSqDist = sqDist;
      }
    }
  }
  return best;
}

/** \p mask cropped to \p rect, which must lie within it. */
BinaryImage crop(const BinaryImage& mask, const QRect& rect) {
  if (rect == mask.rect()) {
    return mask;
  }
  BinaryImage cropped(rect.size(), WHITE);
  rasterOp<RopSrc>(cropped, cropped.rect(), mask, rect.topLeft());
  return cropped;
}

/** Combines the black pixels of \p src, placed at \p srcOrigin, into \p dst, placed at \p dstOrigin. */
template <typename Rop>
void combine(BinaryImage& dst, const QPoint& dstOrigin, const BinaryImage& src, const QPoint& srcOrigin) {
  const QPoint offset(srcOrigin - dstOrigin);
  const QRect dstRect(QRect(offset, src.size()).intersected(dst.rect()));
  if (!dstRect.isEmpty()) {
    rasterOp<Rop>(dst, dstRect, src, dstRect.topLeft() - offset);
  }
}
}  // namespace

/*================================ Selection ================================*/

Selection::Selection(const BinaryImage& mask, const QPoint& origin) : m_mask(mask), m_origin(origin) {
  trim();
}

QRect Selection::rect() const {
  return isNull() ? QRect() : QRect(m_origin, m_mask.size());
}

void Selection::trim() {
  if (m_mask.isNull()) {
    return;
  }
  const QRect box(m_mask.contentBoundingBox(BLACK));
  if (box.isEmpty()) {
    m_mask = BinaryImage();
    m_origin = QPoint();
    return;
  }
  m_mask = crop(m_mask, box);
  m_origin += box.topLeft();
}

Selection Selection::united(const Selection& other) const {
  if (other.isNull()) {
    return *this;
  }
  if (isNull()) {
    return other;
  }
  const QRect area(rect().united(other.rect()));
  BinaryImage merged(area.size(), WHITE);
  combine<RopOr<RopSrc, RopDst>>(merged, area.topLeft(), m_mask, m_origin);
  combine<RopOr<RopSrc, RopDst>>(merged, area.topLeft(), other.m_mask, other.m_origin);
  return Selection(merged, area.topLeft());
}

Selection Selection::subtracted(const Selection& other) const {
  if (isNull() || other.isNull() || !rect().intersects(other.rect())) {
    return *this;
  }
  BinaryImage remaining(m_mask);
  combine<RopSubtract<RopDst, RopSrc>>(remaining, m_origin, other.m_mask, other.m_origin);
  return Selection(remaining, m_origin);
}

/*============================== selectObject ===============================*/

QColor estimatePaperColor(const QImage& image, const QRect& area) {
  const QRect rect(area.intersected(image.rect()));
  if (rect.isEmpty()) {
    return QColor(Qt::white);
  }
  // Some 65 thousand pixels are plenty for a median, whatever the size of the area.
  const qint64 pixels = qint64(rect.width()) * rect.height();
  const int step = std::max(1, int(std::sqrt(double(pixels) / 65536.0)));
  std::vector<QRgb> samples;
  samples.reserve(size_t((rect.width() / step + 1) * (rect.height() / step + 1)));
  for (int y = rect.top(); y <= rect.bottom(); y += step) {
    for (int x = rect.left(); x <= rect.right(); x += step) {
      samples.push_back(image.pixel(x, y));
    }
  }
  // Paper is the light majority of a page. The lighter half leaves the print out.
  const auto half = samples.begin() + samples.size() / 2;
  std::nth_element(samples.begin(), half, samples.end(),
                   [](const QRgb a, const QRgb b) { return qGray(a) > qGray(b); });
  samples.erase(half + 1, samples.end());

  std::vector<int> reds, greens, blues;
  for (const QRgb rgb : samples) {
    reds.push_back(qRed(rgb));
    greens.push_back(qGreen(rgb));
    blues.push_back(qBlue(rgb));
  }
  const auto median = [](std::vector<int>& values) {
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
  };
  return QColor(median(reds), median(greens), median(blues));
}

Selection selectObject(const QImage& image, const QPoint& seed, const ObjectSelectionParams& params) {
  DIAG_SCOPE(diagScope, "retouch.select");
  if (image.isNull() || !image.rect().contains(seed)) {
    return Selection();
  }

  const int grouping = std::max(0, params.grouping);
  const int margin = std::max(0, params.margin);
  // Measured once, around the click, so that growing the window cannot change
  // what counts as ink half way through.
  const QColor paper(estimatePaperColor(
      image, QRect(seed - QPoint(INITIAL_REACH, INITIAL_REACH), QSize(2 * INITIAL_REACH, 2 * INITIAL_REACH))));

  for (int reach = INITIAL_REACH;; reach *= 2) {
    const QRect window(
        QRect(seed - QPoint(reach, reach), QSize(2 * reach + 1, 2 * reach + 1)).intersected(image.rect()));
    const bool wholeImage = (window == image.rect());

    const BinaryImage ink(inkMask(image, window, paper, params.tolerance));
    const QPoint start(nearestInk(ink, seed - window.topLeft(), std::max(0, params.seedSearchRadius)));
    if (start.x() < 0) {
      diagScope.attr(core::diag::Attr("found", false));
      return Selection();
    }

    // Ink within the grouping distance of other ink becomes connected to it:
    // grown by half the distance, two pieces that far apart meet in the middle.
    const int growth = (grouping + 1) / 2;
    const BinaryImage reachable(growth > 0 ? dilateBrick(ink, Brick(QSize(2 * growth + 1, 2 * growth + 1))) : ink);
    BinaryImage seedImage(window.size(), WHITE);
    seedImage.setPixel(start.x(), start.y(), BLACK);
    BinaryImage object(seedFill(seedImage, reachable, CONN8));

    // An object that runs into a side of the window that is not the edge of the
    // image may go on past it: look again, further out.
    const QRect objectBox(object.contentBoundingBox(BLACK));
    const bool clipped = ((objectBox.left() == 0) && (window.left() > 0))
                         || ((objectBox.top() == 0) && (window.top() > 0))
                         || ((objectBox.right() == window.width() - 1) && (window.right() < image.width() - 1))
                         || ((objectBox.bottom() == window.height() - 1) && (window.bottom() < image.height() - 1));
    if (clipped && !wholeImage) {
      continue;
    }

    // The ink of the object, without the paper the grouping bridged.
    rasterOp<RopAnd<RopSrc, RopDst>>(object, ink);
    const QRect inkBox(object.contentBoundingBox(BLACK));
    if (inkBox.isEmpty()) {
      return Selection();
    }
    const QRect box(inkBox.adjusted(-margin, -margin, margin, margin).intersected(object.rect()));
    BinaryImage selected(crop(object, box));
    if (margin > 0) {
      selected = dilateBrick(selected, Brick(QSize(2 * margin + 1, 2 * margin + 1)));
    }
    diagScope.attr(core::diag::Attr("found", true));
    diagScope.attr(core::diag::Attr("reach", reach));
    return Selection(selected, window.topLeft() + box.topLeft());
  }
}
}  // namespace retouch
