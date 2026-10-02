// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "OutputLayer.h"

#include <QByteArray>
#include <QDomDocument>
#include <QDomElement>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <QtGlobal>
#include <cmath>
#include <cstdint>

using namespace imageproc;

namespace retouch {
namespace {
/** Close enough to doing nothing that the edits are best painted as they are, unresampled. */
bool isNearlyIdentity(const QTransform& transform) {
  const auto near = [](const double value, const double target, const double tolerance) {
    return std::abs(value - target) <= tolerance;
  };
  return (transform.type() <= QTransform::TxRotate) && near(transform.m11(), 1.0, 1e-6) && near(transform.m22(), 1.0, 1e-6)
         && near(transform.m12(), 0.0, 1e-6) && near(transform.m21(), 0.0, 1e-6) && near(transform.dx(), 0.0, 0.01)
         && near(transform.dy(), 0.0, 0.01);
}

QString number(const double value) {
  return QString::number(value, 'g', 17);
}

/** The rows of \p mask, each word as four bytes, most significant first, compressed and in base64. */
QString maskToText(const BinaryImage& mask) {
  const int wpl = mask.wordsPerLine();
  QByteArray bytes;
  bytes.reserve(mask.height() * wpl * 4);
  const uint32_t* line = mask.data();
  for (int y = 0; y < mask.height(); ++y, line += wpl) {
    for (int i = 0; i < wpl; ++i) {
      const uint32_t word = line[i];
      bytes.append(char(word >> 24));
      bytes.append(char((word >> 16) & 0xff));
      bytes.append(char((word >> 8) & 0xff));
      bytes.append(char(word & 0xff));
    }
  }
  return QString::fromLatin1(qCompress(bytes, 9).toBase64());
}

/** The inverse of maskToText(); a null image if \p text does not hold a \p size mask. */
BinaryImage maskFromText(const QString& text, const QSize& size) {
  if (size.isEmpty()) {
    return BinaryImage();
  }
  const QByteArray bytes(qUncompress(QByteArray::fromBase64(text.toLatin1())));
  BinaryImage mask(size, WHITE);
  const int wpl = mask.wordsPerLine();
  if (bytes.size() != size.height() * wpl * 4) {
    return BinaryImage();
  }
  const auto* in = reinterpret_cast<const uchar*>(bytes.constData());
  uint32_t* line = mask.data();
  for (int y = 0; y < size.height(); ++y, line += wpl) {
    for (int i = 0; i < wpl; ++i, in += 4) {
      line[i] = (uint32_t(in[0]) << 24) | (uint32_t(in[1]) << 16) | (uint32_t(in[2]) << 8) | uint32_t(in[3]);
    }
  }
  return mask;
}

QDomElement editToXml(QDomDocument& doc, const Edit& edit) {
  QDomElement el;
  switch (edit.kind()) {
    case Edit::RECT:
      el = doc.createElement("rect");
      el.setAttribute("x", edit.rect().x());
      el.setAttribute("y", edit.rect().y());
      el.setAttribute("width", edit.rect().width());
      el.setAttribute("height", edit.rect().height());
      break;
    case Edit::STROKE: {
      el = doc.createElement("stroke");
      el.setAttribute("diameter", number(edit.diameter()));
      QStringList points;
      for (const QPointF& point : edit.points()) {
        points << number(point.x()) + QLatin1Char(',') + number(point.y());
      }
      el.setAttribute("points", points.join(QLatin1Char(' ')));
      break;
    }
    case Edit::MASK:
      el = doc.createElement("mask");
      el.setAttribute("x", edit.maskOrigin().x());
      el.setAttribute("y", edit.maskOrigin().y());
      el.setAttribute("width", edit.maskImage().width());
      el.setAttribute("height", edit.maskImage().height());
      el.setAttribute("bits", maskToText(edit.maskImage()));
      break;
  }
  el.setAttribute("color", edit.color().name());
  if (edit.colorFromPaper()) {
    el.setAttribute("paper", 1);
  }
  return el;
}

/** \return false for an element that does not describe an edit, which is then skipped. */
bool editFromXml(const QDomElement& el, std::vector<Edit>* edits) {
  const QColor color(el.attribute("color"));
  if (!color.isValid()) {
    return false;
  }
  const bool fromPaper = el.attribute("paper").toInt() != 0;
  const QString kind(el.tagName());
  if (kind == QLatin1String("rect")) {
    const QRect rect(el.attribute("x").toInt(), el.attribute("y").toInt(), el.attribute("width").toInt(),
                     el.attribute("height").toInt());
    if (rect.isEmpty()) {
      return false;
    }
    edits->push_back(Edit::rect(rect, color).withColor(color, fromPaper));
    return true;
  }
  if (kind == QLatin1String("stroke")) {
    QPolygonF points;
    for (const QString& pair : el.attribute("points").split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
      const QStringList xy(pair.split(QLatin1Char(',')));
      if (xy.size() == 2) {
        points << QPointF(xy[0].toDouble(), xy[1].toDouble());
      }
    }
    const double diameter = el.attribute("diameter").toDouble();
    if (points.isEmpty() || (diameter <= 0)) {
      return false;
    }
    edits->push_back(Edit::stroke(points, diameter, color).withColor(color, fromPaper));
    return true;
  }
  if (kind == QLatin1String("mask")) {
    const QSize size(el.attribute("width").toInt(), el.attribute("height").toInt());
    const BinaryImage mask(maskFromText(el.attribute("bits"), size));
    if (mask.isNull()) {
      return false;
    }
    const QPoint origin(el.attribute("x").toInt(), el.attribute("y").toInt());
    edits->push_back(Edit::mask(mask, origin, color).withColor(color, fromPaper));
    return true;
  }
  return false;
}

/** \p edits, with the colours taken from the paper taken again from \p image's. */
std::vector<Edit> withPaperFrom(const std::vector<Edit>& edits, const QImage& image, const int paperRing) {
  std::vector<Edit> result;
  result.reserve(edits.size());
  for (const Edit& edit : edits) {
    if (edit.colorFromPaper()) {
      const QRect area(edit.boundingRect().intersected(image.rect()));
      if (!area.isEmpty()) {
        const QColor paper(surroundingColor(image, edit.coverage(area), area.topLeft(), paperRing));
        if (paper.isValid()) {
          result.push_back(edit.withColor(paper, true));
          continue;
        }
      }
    }
    result.push_back(edit);
  }
  return result;
}
}  // namespace

OutputLayer::OutputLayer(const QDomElement& el) {
  m_outputSize = QSize(el.attribute("width").toInt(), el.attribute("height").toInt());
  m_dewarped = el.attribute("dewarped").toInt() != 0;
  const QDomElement xformEl(el.namedItem("transform").toElement());
  m_originalToOutput = QTransform(xformEl.attribute("m11", "1").toDouble(), xformEl.attribute("m12", "0").toDouble(),
                                  xformEl.attribute("m21", "0").toDouble(), xformEl.attribute("m22", "1").toDouble(),
                                  xformEl.attribute("dx", "0").toDouble(), xformEl.attribute("dy", "0").toDouble());
  for (QDomNode node(el.firstChild()); !node.isNull(); node = node.nextSibling()) {
    const QDomElement child(node.toElement());
    if (!child.isNull() && (child.tagName() != QLatin1String("transform"))) {
      editFromXml(child, &m_edits);
    }
  }
}

QDomElement OutputLayer::toXml(QDomDocument& doc, const QString& name) const {
  QDomElement el(doc.createElement(name));
  el.setAttribute("width", m_outputSize.width());
  el.setAttribute("height", m_outputSize.height());
  if (m_dewarped) {
    el.setAttribute("dewarped", 1);
  }
  QDomElement xformEl(doc.createElement("transform"));
  xformEl.setAttribute("m11", number(m_originalToOutput.m11()));
  xformEl.setAttribute("m12", number(m_originalToOutput.m12()));
  xformEl.setAttribute("m21", number(m_originalToOutput.m21()));
  xformEl.setAttribute("m22", number(m_originalToOutput.m22()));
  xformEl.setAttribute("dx", number(m_originalToOutput.dx()));
  xformEl.setAttribute("dy", number(m_originalToOutput.dy()));
  el.appendChild(xformEl);
  for (const Edit& edit : m_edits) {
    el.appendChild(editToXml(doc, edit));
  }
  return el;
}

OutputLayer OutputLayer::adding(const std::vector<Edit>& edits,
                                const QTransform& originalToOutput,
                                const QSize& outputSize,
                                const bool dewarped,
                                bool* dropped) const {
  OutputLayer result;
  result.m_originalToOutput = originalToOutput;
  result.m_outputSize = outputSize;
  result.m_dewarped = dewarped;
  bool lost = false;
  if (!isEmpty() && !placed(originalToOutput, outputSize, dewarped, &result.m_edits)) {
    lost = true;
  }
  result.m_edits.insert(result.m_edits.end(), edits.begin(), edits.end());
  if (dropped) {
    *dropped = lost;
  }
  return result;
}

bool OutputLayer::placed(const QTransform& originalToOutput,
                         const QSize& outputSize,
                         const bool dewarped,
                         std::vector<Edit>* edits) const {
  bool invertible = false;
  const QTransform outputToOriginal(m_originalToOutput.inverted(&invertible));
  if (!invertible) {
    return false;
  }
  // From the pixels of the output the edits were made on to those of this one.
  const QTransform move(outputToOriginal * originalToOutput);
  const bool unmoved = isNearlyIdentity(move);
  if ((m_dewarped || dewarped) && (!unmoved || (m_dewarped != dewarped) || (m_outputSize != outputSize))) {
    return false;
  }
  *edits = unmoved ? m_edits : transformed(m_edits, move);
  return true;
}

bool OutputLayer::applyTo(QImage& output,
                          const QTransform& originalToOutput,
                          const bool dewarped,
                          const int paperRing) const {
  if (isEmpty()) {
    return true;
  }
  std::vector<Edit> edits;
  if (!placed(originalToOutput, output.size(), dewarped, &edits)) {
    return false;
  }
  applyEdits(output, withPaperFrom(edits, output, paperRing));
  return true;
}

std::vector<Edit> transformed(const std::vector<Edit>& edits, const QTransform& transform) {
  std::vector<Edit> result;
  result.reserve(edits.size());
  for (const Edit& edit : edits) {
    const QRect area(transform.mapRect(QRectF(edit.boundingRect())).toAlignedRect().adjusted(-1, -1, 1, 1));
    if (area.isEmpty()) {
      continue;
    }
    QImage canvas(area.size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
      QPainter painter(&canvas);
      painter.setRenderHint(QPainter::Antialiasing, true);
      painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
      painter.translate(-area.topLeft());
      painter.setTransform(transform, true);
      switch (edit.kind()) {
        case Edit::RECT:
          painter.fillRect(QRectF(edit.rect()), Qt::black);
          break;
        case Edit::STROKE:
          paintStroke(painter, edit.points(), edit.diameter(), Qt::black);
          break;
        case Edit::MASK:
          painter.drawImage(edit.maskOrigin(), edit.maskImage().toAlphaMask(Qt::black));
          break;
      }
    }

    // Every pixel the moved shape reaches at all.
    BinaryImage mask(area.size(), WHITE);
    const int wpl = mask.wordsPerLine();
    uint32_t* line = mask.data();
    bool any = false;
    for (int y = 0; y < area.height(); ++y, line += wpl) {
      const auto* pixels = reinterpret_cast<const QRgb*>(canvas.constScanLine(y));
      for (int x = 0; x < area.width(); ++x) {
        if (qAlpha(pixels[x]) != 0) {
          line[x >> 5] |= uint32_t(0x80000000) >> (x & 31);
          any = true;
        }
      }
    }
    if (any) {
      result.push_back(Edit::mask(mask, area.topLeft(), edit.color()).withColor(edit.color(), edit.colorFromPaper()));
    }
  }
  return result;
}
}  // namespace retouch
