// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "Edit.h"

#include <Morphology.h>
#include <RasterOp.h>

#include <QImage>
#include <QPainter>
#include <QPen>
#include <QtGlobal>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>

using namespace imageproc;

namespace retouch {
namespace {
/** Calls \p fn(x, y) for every black pixel of \p mask, skipping empty words. */
template <typename Fn>
void forEachBlackPixel(const BinaryImage& mask, Fn fn) {
  const int width = mask.width();
  const int height = mask.height();
  const int wpl = mask.wordsPerLine();
  const uint32_t* line = mask.data();
  for (int y = 0; y < height; ++y, line += wpl) {
    for (int wordIdx = 0; wordIdx < wpl; ++wordIdx) {
      uint32_t word = line[wordIdx];
      const int x0 = wordIdx << 5;
      // The most significant bit is the leftmost pixel of the word.
      for (int bit = 0; word; ++bit) {
        const uint32_t bitMask = uint32_t(0x80000000) >> bit;
        if (!(word & bitMask)) {
          continue;
        }
        word &= ~bitMask;
        // The bits past the last pixel of a line are not guaranteed to be clear.
        const int x = x0 + bit;
        if (x < width) {
          fn(x, y);
        }
      }
    }
  }
}

bool allGray(const QVector<QRgb>& table) {
  for (const QRgb rgb : table) {
    if ((qRed(rgb) != qGreen(rgb)) || (qGreen(rgb) != qBlue(rgb))) {
      return false;
    }
  }
  return true;
}

/** The entry of the colour table of \p image closest to \p rgb. */
int nearestColorIndex(const QImage& image, const QRgb rgb) {
  const QVector<QRgb> table(image.colorTable());
  if (table.isEmpty()) {
    return 0;
  }
  // A grey palette is matched on lightness alone. Matched on RGB distance, a
  // cream paper colour would come out a shade darker than the grey of the same
  // lightness, and the patch would show.
  const bool gray = allGray(table);
  int best = 0;
  long bestDist = LONG_MAX;
  for (int i = 0; i < table.size(); ++i) {
    long dist;
    if (gray) {
      dist = std::abs(qGray(table[i]) - qGray(rgb));
    } else {
      const long dr = qRed(table[i]) - qRed(rgb);
      const long dg = qGreen(table[i]) - qGreen(rgb);
      const long db = qBlue(table[i]) - qBlue(rgb);
      // The usual weights for perceived difference: green counts most.
      dist = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
    }
    if (dist < bestDist) {
      best = i;
      bestDist = dist;
    }
  }
  return best;
}

/** Writes \p requested into every pixel of \p image under the black pixels of \p cover, placed at \p origin. */
void paintCovered(QImage& image, const BinaryImage& cover, const QPoint& origin, const QColor& requested) {
  const QColor color(effectiveColor(image, requested));
  const QRgb rgb = color.rgb();
  const int ox = origin.x();
  const int oy = origin.y();

  switch (image.format()) {
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB: {
      const bool msbFirst = (image.format() == QImage::Format_Mono);
      const bool set = (nearestColorIndex(image, rgb) != 0);
      uchar* const bits = image.bits();
      const ptrdiff_t bpl = image.bytesPerLine();
      forEachBlackPixel(cover, [&](const int x, const int y) {
        const int ix = ox + x;
        uchar* byte = bits + ptrdiff_t(oy + y) * bpl + (ix >> 3);
        const uchar bitMask = msbFirst ? uchar(0x80 >> (ix & 7)) : uchar(1 << (ix & 7));
        if (set) {
          *byte |= bitMask;
        } else {
          *byte &= uchar(~bitMask);
        }
      });
      break;
    }
    case QImage::Format_Indexed8:
    case QImage::Format_Grayscale8: {
      const auto value = static_cast<uchar>((image.format() == QImage::Format_Indexed8) ? nearestColorIndex(image, rgb)
                                                                                        : qGray(rgb));
      uchar* const bits = image.bits();
      const ptrdiff_t bpl = image.bytesPerLine();
      forEachBlackPixel(cover, [&](const int x, const int y) { bits[ptrdiff_t(oy + y) * bpl + ox + x] = value; });
      break;
    }
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied: {
      // Opaque, so premultiplied or not makes no difference.
      const uint32_t value = 0xff000000u | (rgb & 0x00ffffffu);
      uchar* const bits = image.bits();
      const ptrdiff_t bpl = image.bytesPerLine();
      forEachBlackPixel(cover, [&](const int x, const int y) {
        reinterpret_cast<uint32_t*>(bits + ptrdiff_t(oy + y) * bpl)[ox + x] = value;
      });
      break;
    }
    case QImage::Format_RGB888: {
      uchar* const bits = image.bits();
      const ptrdiff_t bpl = image.bytesPerLine();
      forEachBlackPixel(cover, [&](const int x, const int y) {
        uchar* p = bits + ptrdiff_t(oy + y) * bpl + 3 * (ox + x);
        p[0] = static_cast<uchar>(qRed(rgb));
        p[1] = static_cast<uchar>(qGreen(rgb));
        p[2] = static_cast<uchar>(qBlue(rgb));
      });
      break;
    }
    default:
      // The rarer formats - 16 bits per channel, for one - go through Qt's own
      // conversion. Slower per pixel, but exact for every format it can read.
      forEachBlackPixel(cover, [&](const int x, const int y) { image.setPixelColor(ox + x, oy + y, color); });
      break;
  }
}

int medianOf(std::vector<int>& values) {
  const auto middle = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

QColor medianColor(const std::vector<QRgb>& colors) {
  std::vector<int> reds, greens, blues;
  reds.reserve(colors.size());
  greens.reserve(colors.size());
  blues.reserve(colors.size());
  for (const QRgb rgb : colors) {
    reds.push_back(qRed(rgb));
    greens.push_back(qGreen(rgb));
    blues.push_back(qBlue(rgb));
  }
  return QColor(medianOf(reds), medianOf(greens), medianOf(blues));
}

/**
 * \p mask, placed at \p origin, as it covers an image \p sx and \p sy times the
 * size: a pixel is black if any black source pixel overlaps it.
 */
BinaryImage scaleCovering(const BinaryImage& mask,
                          const QPoint& origin,
                          const double sx,
                          const double sy,
                          QPoint* newOrigin) {
  // A little slack, so that rounding never takes in a neighbouring pixel.
  const double eps = 1e-7;
  const int left = int(std::floor(origin.x() * sx));
  const int top = int(std::floor(origin.y() * sy));
  const int right = int(std::ceil((origin.x() + mask.width()) * sx));
  const int bottom = int(std::ceil((origin.y() + mask.height()) * sy));
  *newOrigin = QPoint(left, top);
  const int width = std::max(1, right - left);
  const int height = std::max(1, bottom - top);

  // The source pixels [first, last) under target column or row i.
  const auto range = [eps](const int i, const double scale, const int origin, const int limit) {
    const int first = std::max(0, int(std::floor(i / scale + eps)) - origin);
    const int last = std::min(limit, int(std::ceil((i + 1) / scale - eps)) - origin);
    return std::make_pair(first, std::max(first + 1, last));
  };

  // Columns first, then rows: a black pixel anywhere in the rectangle under a
  // target pixel is a black pixel in its row's span, in one of its rows.
  BinaryImage columns(QSize(width, mask.height()), WHITE);
  for (int u = 0; u < width; ++u) {
    const auto span = range(left + u, sx, origin.x(), mask.width());
    for (int y = 0; y < mask.height(); ++y) {
      for (int x = span.first; (x < span.second) && (x < mask.width()); ++x) {
        if (mask.getPixel(x, y) == BLACK) {
          columns.setPixel(u, y, BLACK);
          break;
        }
      }
    }
  }
  BinaryImage scaled(QSize(width, height), WHITE);
  for (int v = 0; v < height; ++v) {
    const auto span = range(top + v, sy, origin.y(), mask.height());
    for (int u = 0; u < width; ++u) {
      for (int y = span.first; (y < span.second) && (y < mask.height()); ++y) {
        if (columns.getPixel(u, y) == BLACK) {
          scaled.setPixel(u, v, BLACK);
          break;
        }
      }
    }
  }
  return scaled;
}
}  // namespace

Edit::Edit(const Kind kind, const QColor& color)
    : m_kind(kind), m_color(color), m_colorFromPaper(false), m_diameter(0) {}

Edit Edit::rect(const QRect& rect, const QColor& color) {
  Edit edit(RECT, color);
  edit.m_rect = rect.normalized();
  return edit;
}

Edit Edit::stroke(const QPolygonF& points, const double diameter, const QColor& color) {
  Edit edit(STROKE, color);
  edit.m_points = points;
  edit.m_diameter = std::max(1.0, diameter);
  return edit;
}

Edit Edit::mask(const BinaryImage& mask, const QPoint& origin, const QColor& color) {
  Edit edit(MASK, color);
  edit.m_mask = mask;
  edit.m_maskOrigin = origin;
  return edit;
}

Edit Edit::withColor(const QColor& color, const bool fromPaper) const {
  Edit edit(*this);
  edit.m_color = color;
  edit.m_colorFromPaper = fromPaper;
  return edit;
}

Edit Edit::scaled(const double sx, const double sy) const {
  if ((sx == 1.0) && (sy == 1.0)) {
    return *this;
  }
  Edit edit(*this);
  switch (m_kind) {
    case RECT: {
      const QPoint topLeft(int(std::floor(m_rect.left() * sx)), int(std::floor(m_rect.top() * sy)));
      const QPoint bottomRight(int(std::ceil((m_rect.right() + 1) * sx)) - 1,
                               int(std::ceil((m_rect.bottom() + 1) * sy)) - 1);
      edit.m_rect = QRect(topLeft, bottomRight);
      break;
    }
    case STROKE:
      for (QPointF& point : edit.m_points) {
        point = QPointF(point.x() * sx, point.y() * sy);
      }
      // A pixel wider, for what rounding would otherwise leave at the edges.
      edit.m_diameter = m_diameter * std::sqrt(sx * sy) + 1.0;
      break;
    case MASK:
      if (!m_mask.isNull()) {
        edit.m_mask = scaleCovering(m_mask, m_maskOrigin, sx, sy, &edit.m_maskOrigin);
      }
      break;
  }
  return edit;
}

QRect Edit::boundingRect() const {
  switch (m_kind) {
    case RECT:
      return m_rect;
    case STROKE: {
      if (m_points.isEmpty()) {
        return QRect();
      }
      // One pixel more than the radius: rasterization rounds, and a pixel left
      // out of the area here would be left out of the edit.
      const double reach = m_diameter / 2.0 + 1.0;
      return m_points.boundingRect().adjusted(-reach, -reach, reach, reach).toAlignedRect();
    }
    case MASK:
      return m_mask.isNull() ? QRect() : QRect(m_maskOrigin, m_mask.size());
  }
  return QRect();
}

BinaryImage Edit::coverage(const QRect& area) const {
  if (area.isEmpty()) {
    return BinaryImage();
  }

  switch (m_kind) {
    case RECT: {
      BinaryImage cover(area.size(), WHITE);
      const QRect covered(m_rect.translated(-area.topLeft()).intersected(cover.rect()));
      if (!covered.isEmpty()) {
        cover.fill(covered, BLACK);
      }
      return cover;
    }
    case STROKE: {
      // Painted, not computed: QPainter draws the on-screen preview of the same
      // stroke, and using it here too is what keeps the two in agreement.
      QImage canvas(area.size(), QImage::Format_Mono);
      canvas.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});
      canvas.fill(0);
      {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.translate(-area.x(), -area.y());
        paintStroke(painter, m_points, m_diameter, Qt::black);
      }
      return BinaryImage(canvas);
    }
    case MASK: {
      const QRect maskRect(boundingRect());
      if (maskRect == area) {
        return m_mask;
      }
      BinaryImage cover(area.size(), WHITE);
      const QPoint offset(m_maskOrigin - area.topLeft());
      const QRect dstRect(QRect(offset, m_mask.size()).intersected(cover.rect()));
      if (!dstRect.isEmpty()) {
        rasterOp<RopSrc>(cover, dstRect, m_mask, dstRect.topLeft() - offset);
      }
      return cover;
    }
  }
  return BinaryImage();
}

void paintStroke(QPainter& painter, const QPolygonF& points, const double diameter, const QColor& color) {
  if (points.isEmpty()) {
    return;
  }
  if (points.size() == 1) {
    // A zero-length line is drawn as nothing at all by some paint engines.
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const double radius = diameter / 2.0;
    painter.drawEllipse(points.front(), radius, radius);
    return;
  }
  QPen pen(color);
  pen.setWidthF(diameter);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawPolyline(points);
}

QColor effectiveColor(const QImage& image, const QColor& requested) {
  const QRgb rgb = requested.rgb();
  switch (image.format()) {
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB:
    case QImage::Format_Indexed8:
      if (image.colorCount() > 0) {
        return QColor(image.color(nearestColorIndex(image, rgb)));
      }
      break;
    case QImage::Format_Grayscale8:
#if QT_VERSION >= QT_VERSION_CHECK(5, 13, 0)
    case QImage::Format_Grayscale16:
#endif
    {
      const int gray = qGray(rgb);
      return QColor(gray, gray, gray);
    }
    default:
      break;
  }
  return QColor(qRed(rgb), qGreen(rgb), qBlue(rgb));
}

void applyEdits(QImage& image, const std::vector<Edit>& edits) {
  if (image.isNull()) {
    return;
  }
  const QRect imageRect(image.rect());
  for (const Edit& edit : edits) {
    const QRect area(edit.boundingRect().intersected(imageRect));
    if (area.isEmpty()) {
      continue;
    }
    paintCovered(image, edit.coverage(area), area.topLeft(), edit.color());
  }
}

std::vector<Edit> carryOver(const std::vector<Edit>& edits,
                            const double sx,
                            const double sy,
                            const QImage& target,
                            const int paperRing) {
  std::vector<Edit> carried;
  carried.reserve(edits.size());
  for (const Edit& edit : edits) {
    Edit moved(edit.scaled(sx, sy));
    if (moved.colorFromPaper()) {
      const QRect area(moved.boundingRect().intersected(target.rect()));
      if (!area.isEmpty()) {
        const QColor paper(surroundingColor(target, moved.coverage(area), area.topLeft(), paperRing));
        if (paper.isValid()) {
          moved = moved.withColor(paper, true);
        }
      }
    }
    carried.push_back(moved);
  }
  return carried;
}

QColor surroundingColor(const QImage& image, const BinaryImage& coverage, const QPoint& origin, const int ringWidth) {
  if (image.isNull() || coverage.isNull() || (ringWidth < 1)) {
    return QColor();
  }
  const QRect coverRect(origin, coverage.size());
  const QRect outer(coverRect.adjusted(-ringWidth, -ringWidth, ringWidth, ringWidth).intersected(image.rect()));
  if (outer.isEmpty()) {
    return QColor();
  }

  // The coverage, moved into the frame of the area around it.
  BinaryImage placed(outer.size(), WHITE);
  const QPoint offset(coverRect.topLeft() - outer.topLeft());
  const QRect dstRect(QRect(offset, coverage.size()).intersected(placed.rect()));
  if (!dstRect.isEmpty()) {
    rasterOp<RopSrc>(placed, dstRect, coverage, dstRect.topLeft() - offset);
  }

  BinaryImage ring(dilateBrick(placed, Brick(QSize(2 * ringWidth + 1, 2 * ringWidth + 1))));
  rasterOp<RopSubtract<RopDst, RopSrc>>(ring, placed);

  // A long ring holds far more pixels than a median needs.
  const int total = ring.countBlackPixels();
  if (total == 0) {
    return QColor();
  }
  const int stride = std::max(1, total / 20000);
  std::vector<QRgb> samples;
  samples.reserve(std::min(total, 20000) + 1);
  int counter = 0;
  forEachBlackPixel(ring, [&](const int x, const int y) {
    if ((counter++ % stride) == 0) {
      samples.push_back(image.pixel(outer.x() + x, outer.y() + y));
    }
  });

  // Ink crossing the band - the edge of the stamp being covered, a line of text
  // running past - is darker than the paper. The lighter part is the paper.
  std::sort(samples.begin(), samples.end(), [](const QRgb a, const QRgb b) { return qGray(a) > qGray(b); });
  samples.resize(std::max<size_t>(1, samples.size() * 6 / 10));
  return medianColor(samples);
}

QColor sampleColor(const QImage& image, const QPoint& center, const int radius) {
  const QRect area(
      QRect(center - QPoint(radius, radius), QSize(2 * radius + 1, 2 * radius + 1)).intersected(image.rect()));
  if (area.isEmpty()) {
    return QColor();
  }
  std::vector<QRgb> samples;
  samples.reserve(area.width() * area.height());
  for (int y = area.top(); y <= area.bottom(); ++y) {
    for (int x = area.left(); x <= area.right(); ++x) {
      samples.push_back(image.pixel(x, y));
    }
  }
  return medianColor(samples);
}
}  // namespace retouch
