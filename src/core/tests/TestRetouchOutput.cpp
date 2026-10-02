// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <BinaryImage.h>
#include <OutputLayer.h>

#include <QDomDocument>
#include <QImage>
#include <QPainter>
#include <boost/test/unit_test.hpp>

using imageproc::BinaryImage;
using imageproc::BLACK;
using imageproc::WHITE;
using retouch::Edit;
using retouch::OutputLayer;

namespace Tests {
namespace {
/** A white output with a black "stamp" square at \p stamp. */
QImage outputWithStamp(const QRect& stamp, const QSize& size = QSize(200, 120)) {
  QImage image(size, QImage::Format_RGB32);
  image.fill(Qt::white);
  QPainter painter(&image);
  painter.fillRect(stamp, Qt::black);
  return image;
}

int blackPixels(const QImage& image, const QRect& area) {
  int count = 0;
  for (int y = area.top(); y <= area.bottom(); ++y) {
    for (int x = area.left(); x <= area.right(); ++x) {
      if (qGray(image.pixel(x, y)) < 128) {
        ++count;
      }
    }
  }
  return count;
}

BinaryImage blob(const QSize& size) {
  BinaryImage mask(size, WHITE);
  for (int y = 1; y < size.height() - 1; ++y) {
    for (int x = 1; x < size.width() - 1; ++x) {
      if ((x + y) % 3 != 0) {
        mask.data()[y * mask.wordsPerLine() + (x >> 5)] |= uint32_t(0x80000000) >> (x & 31);
      }
    }
  }
  return mask;
}

const QTransform SOURCE_TO_OUTPUT(QTransform::fromScale(0.5, 0.5) * QTransform::fromTranslate(-10, -4));
}  // namespace

BOOST_AUTO_TEST_SUITE(RetouchOutputTestSuite)

// Kept in the project: every kind of edit comes back exactly as it went in.
BOOST_AUTO_TEST_CASE(layer_survives_the_project_file) {
  const BinaryImage mask(blob(QSize(37, 9)));
  const std::vector<Edit> edits{Edit::rect(QRect(5, 6, 30, 20), Qt::white),
                                Edit::stroke(QPolygonF({QPointF(1.25, 2.5), QPointF(40.75, 3.5)}), 6.5, Qt::red)
                                    .withColor(QColor(250, 245, 230), true),
                                Edit::mask(mask, QPoint(70, 80), Qt::white)};
  const OutputLayer layer(OutputLayer().adding(edits, SOURCE_TO_OUTPUT, QSize(200, 120), false));

  QDomDocument doc;
  doc.appendChild(layer.toXml(doc, "retouch"));
  const OutputLayer loaded(QDomDocument(doc).documentElement());

  BOOST_REQUIRE_EQUAL(loaded.edits().size(), edits.size());
  BOOST_CHECK(loaded.edits()[0].kind() == Edit::RECT);
  BOOST_CHECK(loaded.edits()[0].rect() == QRect(5, 6, 30, 20));
  BOOST_CHECK(loaded.edits()[1].kind() == Edit::STROKE);
  BOOST_CHECK(loaded.edits()[1].points() == edits[1].points());
  BOOST_CHECK_EQUAL(loaded.edits()[1].diameter(), 6.5);
  BOOST_CHECK(loaded.edits()[1].colorFromPaper());
  BOOST_CHECK(loaded.edits()[1].color() == QColor(250, 245, 230));
  BOOST_CHECK(loaded.edits()[2].kind() == Edit::MASK);
  BOOST_CHECK(loaded.edits()[2].maskOrigin() == QPoint(70, 80));
  BOOST_CHECK(loaded.edits()[2].maskImage() == mask);

  // Placed on an output made the same way, they are what was saved.
  std::vector<Edit> placed;
  BOOST_REQUIRE(loaded.placed(SOURCE_TO_OUTPUT, QSize(200, 120), false, &placed));
  BOOST_CHECK(placed[0].kind() == Edit::RECT);
}

// Made again the same way - another threshold, say - the output gets the
// retouching exactly where it was painted.
BOOST_AUTO_TEST_CASE(retouching_is_painted_over_the_output_made_again) {
  const QRect stamp(120, 60, 40, 25);
  const OutputLayer layer(
      OutputLayer().adding({Edit::rect(stamp.adjusted(-2, -2, 2, 2), Qt::white)}, SOURCE_TO_OUTPUT, QSize(200, 120),
                           false));

  QImage output(outputWithStamp(stamp));
  BOOST_REQUIRE(layer.applyTo(output, SOURCE_TO_OUTPUT, false, 4));
  BOOST_CHECK_EQUAL(blackPixels(output, output.rect()), 0);
  BOOST_CHECK(output.format() == QImage::Format_RGB32);
}

// New margins move the page within its output: the retouching moves with it.
BOOST_AUTO_TEST_CASE(retouching_moves_with_the_page) {
  const QRect stamp(120, 60, 40, 25);
  const OutputLayer layer(
      OutputLayer().adding({Edit::rect(stamp.adjusted(-2, -2, 2, 2), Qt::white)}, SOURCE_TO_OUTPUT, QSize(200, 120),
                           false));

  const QTransform moved(SOURCE_TO_OUTPUT * QTransform::fromTranslate(15, -20));
  QImage output(outputWithStamp(stamp.translated(15, -20), QSize(230, 120)));
  // Something that was never painted over stays.
  {
    QPainter painter(&output);
    painter.fillRect(QRect(10, 10, 20, 20), Qt::black);
  }
  BOOST_REQUIRE(layer.applyTo(output, moved, false, 4));
  BOOST_CHECK_EQUAL(blackPixels(output, QRect(100, 20, 120, 80)), 0);
  BOOST_CHECK_EQUAL(blackPixels(output, QRect(10, 10, 20, 20)), 400);
}

// A dewarped output is no affine image of its source: once its geometry has
// changed, the retouching is not painted at all rather than in the wrong place.
BOOST_AUTO_TEST_CASE(dewarped_output_that_changed_is_left_alone) {
  const QRect stamp(120, 60, 40, 25);
  const OutputLayer layer(
      OutputLayer().adding({Edit::rect(stamp, Qt::white)}, SOURCE_TO_OUTPUT, QSize(200, 120), true));

  QImage same(outputWithStamp(stamp));
  BOOST_CHECK(layer.applyTo(same, SOURCE_TO_OUTPUT, true, 4));
  BOOST_CHECK_EQUAL(blackPixels(same, same.rect()), 0);

  QImage changed(outputWithStamp(stamp.translated(5, 0)));
  BOOST_CHECK(!layer.applyTo(changed, SOURCE_TO_OUTPUT * QTransform::fromTranslate(5, 0), true, 4));
  BOOST_CHECK_EQUAL(blackPixels(changed, changed.rect()), 40 * 25);
}

// Retouching again after the page moved: what was there moves onto the output
// the new edits were painted on, and all of it is kept in its terms.
BOOST_AUTO_TEST_CASE(adding_after_the_page_moved_keeps_both) {
  const OutputLayer first(
      OutputLayer().adding({Edit::rect(QRect(20, 20, 10, 10), Qt::white)}, SOURCE_TO_OUTPUT, QSize(200, 120), false));
  const QTransform moved(SOURCE_TO_OUTPUT * QTransform::fromTranslate(30, 0));
  const OutputLayer both(first.adding({Edit::rect(QRect(100, 50, 10, 10), Qt::white)}, moved, QSize(230, 120), false));
  BOOST_REQUIRE_EQUAL(both.edits().size(), 2u);

  QImage output(QSize(230, 120), QImage::Format_RGB32);
  output.fill(Qt::black);
  BOOST_REQUIRE(both.applyTo(output, moved, false, 4));
  // The first rectangle, moved 30 pixels right, and the second where it was painted.
  BOOST_CHECK_EQUAL(blackPixels(output, QRect(50, 20, 10, 10)), 0);
  BOOST_CHECK_EQUAL(blackPixels(output, QRect(100, 50, 10, 10)), 0);
  BOOST_CHECK_EQUAL(blackPixels(output, QRect(20, 20, 10, 10)), 100);
}

// One-bit output, as black-and-white pages are written: painted in its own format.
BOOST_AUTO_TEST_CASE(black_and_white_output_keeps_its_format) {
  const QRect stamp(50, 30, 20, 10);
  QImage output(outputWithStamp(stamp).convertToFormat(QImage::Format_Mono));
  const OutputLayer layer(
      OutputLayer().adding({Edit::rect(stamp, Qt::white)}, SOURCE_TO_OUTPUT, output.size(), false));
  BOOST_REQUIRE(layer.applyTo(output, SOURCE_TO_OUTPUT, false, 4));
  BOOST_CHECK(output.format() == QImage::Format_Mono);
  BOOST_CHECK_EQUAL(blackPixels(output, output.rect()), 0);
}

// Painted on the output, carried back into the input image through the crop,
// the skew and the resolution the output was made with: it lands on what it
// covered there, and on nothing else.
BOOST_AUTO_TEST_CASE(output_edits_land_on_the_input_through_the_transform) {
  QImage source(1000, 1200, QImage::Format_Grayscale8);
  source.fill(255);
  const QRect stamp(300, 400, 60, 30);
  const QRect text(600, 900, 80, 20);
  {
    QPainter painter(&source);
    painter.fillRect(stamp, Qt::black);
    painter.fillRect(text, Qt::black);
  }

  QTransform toOutput;
  toOutput.rotate(2.0);
  toOutput *= QTransform::fromScale(2.0, 2.0);
  toOutput *= QTransform::fromTranslate(-450, -700);
  // Where the stamp is on the output, with a little to spare, as an operator would drag it.
  const QRect onOutput(toOutput.mapRect(QRectF(stamp)).toAlignedRect().adjusted(-2, -2, 2, 2));

  retouch::applyEdits(source, retouch::transformed({Edit::rect(onOutput, Qt::white)}, toOutput.inverted()));
  BOOST_CHECK_EQUAL(blackPixels(source, stamp), 0);
  BOOST_CHECK_EQUAL(blackPixels(source, text), 80 * 20);
  BOOST_CHECK(source.format() == QImage::Format_Grayscale8);
}

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
