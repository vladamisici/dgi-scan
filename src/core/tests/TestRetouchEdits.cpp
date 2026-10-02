// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <BinaryImage.h>
#include <Edit.h>
#include <Selection.h>

#include <QColor>
#include <QImage>
#include <QPainter>
#include <boost/test/unit_test.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace imageproc;
using namespace retouch;

namespace Tests {
namespace {
const QRgb PAPER = qRgb(238, 232, 218);
const QRgb INK = qRgb(25, 20, 30);

/** A cream page with a stamp - a frame and two lines of "letters" - and a line of text far above it. */
QImage stampedPage() {
  QImage image(1200, 1600, QImage::Format_RGB32);
  image.fill(PAPER);
  QPainter painter(&image);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(INK));
  // A line of text, well clear of the stamp.
  for (int i = 0; i < 20; ++i) {
    painter.drawRect(100 + i * 40, 300, 25, 40);
  }
  // The stamp's frame: four bars, 6 px thick, around 600..1000 x 1100..1300.
  painter.drawRect(600, 1100, 400, 6);
  painter.drawRect(600, 1294, 400, 6);
  painter.drawRect(600, 1100, 6, 200);
  painter.drawRect(994, 1100, 6, 200);
  // Its letters, 8 px apart, 20 px inside the frame.
  for (int row = 0; row < 2; ++row) {
    for (int i = 0; i < 15; ++i) {
      painter.drawRect(630 + i * 22, 1130 + row * 80, 14, 50);
    }
  }
  return image;
}

int countBlack(const BinaryImage& image) {
  return image.isNull() ? 0 : image.countBlackPixels();
}
}  // namespace

BOOST_AUTO_TEST_SUITE(RetouchEditsTestSuite)

BOOST_AUTO_TEST_CASE(rect_coverage_is_clipped_to_the_area) {
  const Edit edit(Edit::rect(QRect(10, 20, 30, 40), Qt::white));
  BOOST_CHECK(edit.boundingRect() == QRect(10, 20, 30, 40));

  const BinaryImage whole(edit.coverage(QRect(0, 0, 100, 100)));
  BOOST_CHECK_EQUAL(countBlack(whole), 30 * 40);
  BOOST_CHECK(whole.getPixel(10, 20) == BLACK);
  BOOST_CHECK(whole.getPixel(39, 59) == BLACK);
  BOOST_CHECK(whole.getPixel(40, 59) == WHITE);

  const BinaryImage part(edit.coverage(QRect(30, 50, 100, 100)));
  BOOST_CHECK_EQUAL(countBlack(part), 10 * 10);
}

BOOST_AUTO_TEST_CASE(stroke_coverage_has_the_area_of_a_capsule) {
  const double diameter = 20.0;
  const Edit dot(Edit::stroke(QPolygonF() << QPointF(50.5, 50.5), diameter, Qt::white));
  const double discArea = M_PI * 10.0 * 10.0;
  const int dotPixels = countBlack(dot.coverage(dot.boundingRect()));
  BOOST_CHECK_CLOSE(double(dotPixels), discArea, 8.0);

  const Edit line(Edit::stroke(QPolygonF() << QPointF(20.5, 30.5) << QPointF(220.5, 30.5), diameter, Qt::white));
  const double capsuleArea = 200.0 * diameter + discArea;
  const int linePixels = countBlack(line.coverage(line.boundingRect()));
  BOOST_CHECK_CLOSE(double(linePixels), capsuleArea, 5.0);
  // The bounding rect really does bound it.
  const QRect box(line.boundingRect());
  BOOST_CHECK(box.contains(QRect(10, 20, 221, 21)));
}

BOOST_AUTO_TEST_CASE(mask_coverage_follows_the_mask) {
  BinaryImage mask(QSize(8, 8), WHITE);
  mask.setPixel(1, 2, BLACK);
  mask.setPixel(7, 7, BLACK);
  const Edit edit(Edit::mask(mask, QPoint(100, 200), Qt::white));
  BOOST_CHECK(edit.boundingRect() == QRect(100, 200, 8, 8));

  const BinaryImage exact(edit.coverage(edit.boundingRect()));
  BOOST_CHECK_EQUAL(countBlack(exact), 2);

  const BinaryImage shifted(edit.coverage(QRect(98, 199, 10, 10)));
  BOOST_CHECK(shifted.getPixel(3, 3) == BLACK);
  BOOST_CHECK(shifted.getPixel(9, 8) == BLACK);
  BOOST_CHECK_EQUAL(countBlack(shifted), 2);
}

BOOST_AUTO_TEST_CASE(colours_are_mapped_to_what_the_format_can_hold) {
  QImage mono(16, 16, QImage::Format_Mono);
  mono.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});
  BOOST_CHECK(effectiveColor(mono, QColor(230, 220, 200)) == QColor(Qt::white));
  BOOST_CHECK(effectiveColor(mono, QColor(40, 40, 40)) == QColor(Qt::black));

  QImage gray(16, 16, QImage::Format_Indexed8);
  QVector<QRgb> ramp;
  for (int i = 0; i < 256; ++i) {
    ramp.push_back(qRgb(i, i, i));
  }
  gray.setColorTable(ramp);
  const QColor cream(255, 240, 200);
  const int lightness = qGray(cream.rgb());
  BOOST_CHECK(effectiveColor(gray, cream) == QColor(lightness, lightness, lightness));

  QImage grayscale8(16, 16, QImage::Format_Grayscale8);
  BOOST_CHECK(effectiveColor(grayscale8, cream) == QColor(lightness, lightness, lightness));

  QImage rgb(16, 16, QImage::Format_RGB32);
  BOOST_CHECK(effectiveColor(rgb, cream) == cream);
}

BOOST_AUTO_TEST_CASE(edits_are_applied_in_the_image_format) {
  // Black and white, white = 0 as in a MINISWHITE TIFF.
  QImage mono(64, 64, QImage::Format_Mono);
  mono.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});
  mono.fill(1);
  applyEdits(mono, {Edit::rect(QRect(8, 8, 16, 16), QColor(250, 245, 230))});
  BOOST_CHECK_EQUAL(mono.pixelIndex(8, 8), 0);
  BOOST_CHECK_EQUAL(mono.pixelIndex(23, 23), 0);
  BOOST_CHECK_EQUAL(mono.pixelIndex(24, 23), 1);
  BOOST_CHECK_EQUAL(mono.pixelIndex(7, 8), 1);
  BOOST_CHECK(mono.format() == QImage::Format_Mono);

  // An inverted grey palette, as TiffReader builds for an 8-bit MINISWHITE TIFF.
  QImage inverted(32, 32, QImage::Format_Indexed8);
  QVector<QRgb> table;
  for (int i = 0; i < 256; ++i) {
    table.push_back(qRgb(255 - i, 255 - i, 255 - i));
  }
  inverted.setColorTable(table);
  inverted.fill(200);
  applyEdits(inverted, {Edit::rect(QRect(0, 0, 4, 4), Qt::white)});
  BOOST_CHECK_EQUAL(inverted.pixelIndex(0, 0), 0);
  BOOST_CHECK_EQUAL(inverted.pixelIndex(4, 4), 200);

  QImage rgb(32, 32, QImage::Format_RGB32);
  rgb.fill(INK);
  applyEdits(rgb, {Edit::rect(QRect(0, 0, 4, 4), QColor(PAPER)),
                   Edit::stroke(QPolygonF() << QPointF(16.5, 16.5), 5, Qt::red)});
  BOOST_CHECK(rgb.pixel(3, 3) == PAPER);
  BOOST_CHECK(rgb.pixel(16, 16) == qRgb(255, 0, 0));
  BOOST_CHECK(rgb.pixel(30, 30) == INK);

  // Later edits paint over earlier ones.
  QImage gray8(32, 32, QImage::Format_Grayscale8);
  gray8.fill(0);
  applyEdits(gray8,
             {Edit::rect(QRect(0, 0, 10, 10), Qt::white), Edit::rect(QRect(5, 5, 10, 10), QColor(100, 100, 100))});
  BOOST_CHECK_EQUAL(qGray(gray8.pixel(2, 2)), 255);
  BOOST_CHECK_EQUAL(qGray(gray8.pixel(7, 7)), 100);
}

BOOST_AUTO_TEST_CASE(edits_reaching_past_the_image_are_clipped) {
  QImage image(20, 20, QImage::Format_RGB32);
  image.fill(INK);
  applyEdits(image, {Edit::rect(QRect(-10, -10, 15, 15), Qt::white),
                     Edit::stroke(QPolygonF() << QPointF(-50, 19.5) << QPointF(80, 19.5), 3, Qt::white)});
  BOOST_CHECK(image.pixel(0, 0) == qRgb(255, 255, 255));
  BOOST_CHECK(image.pixel(4, 4) == qRgb(255, 255, 255));
  BOOST_CHECK(image.pixel(5, 5) == INK);
  BOOST_CHECK(image.pixel(10, 19) == qRgb(255, 255, 255));
}

BOOST_AUTO_TEST_CASE(surrounding_colour_is_the_paper_not_the_ink) {
  const QImage page(stampedPage());
  // Covering one letter: the band around it crosses paper and neighbouring letters.
  BinaryImage cover(QSize(14, 50), BLACK);
  const QColor around(surroundingColor(page, cover, QPoint(630, 1130), 12));
  BOOST_REQUIRE(around.isValid());
  BOOST_CHECK(around == QColor(PAPER));

  const QColor sampled(sampleColor(page, QPoint(50, 50), 3));
  BOOST_CHECK(sampled == QColor(PAPER));
}

BOOST_AUTO_TEST_CASE(paper_colour_is_estimated_from_the_lighter_pixels) {
  const QImage page(stampedPage());
  BOOST_CHECK(estimatePaperColor(page, page.rect()) == QColor(PAPER));
}

BOOST_AUTO_TEST_CASE(selecting_a_letter_takes_the_whole_stamp) {
  const QImage page(stampedPage());
  ObjectSelectionParams params;
  // The letters are 8 px apart, their rows 30 px, and the frame 24 px from them.
  params.grouping = 30;
  params.margin = 2;

  const Selection stamp(selectObject(page, QPoint(637, 1150), params));
  BOOST_REQUIRE(!stamp.isNull());
  // The frame, every letter, and the margin around them.
  BOOST_CHECK(stamp.rect() == QRect(598, 1098, 404, 204));
  // Paper between the letters is not selected.
  // (Between the first two letters, beyond the 2 px margin of either.)
  const QPoint inside(QPoint(648, 1160) - stamp.origin());
  BOOST_CHECK(stamp.mask().getPixel(inside.x(), inside.y()) == WHITE);
  // The line of text far above is not part of it.
  BOOST_CHECK(!stamp.rect().intersects(QRect(0, 0, 1200, 400)));

  // With no grouping, only the letter itself.
  params.grouping = 0;
  const Selection letter(selectObject(page, QPoint(637, 1150), params));
  BOOST_REQUIRE(!letter.isNull());
  BOOST_CHECK(letter.rect() == QRect(628, 1128, 18, 54));
}

BOOST_AUTO_TEST_CASE(clicking_paper_selects_the_ink_next_to_it_or_nothing) {
  const QImage page(stampedPage());
  ObjectSelectionParams params;
  params.grouping = 0;
  params.seedSearchRadius = 8;

  // Four pixels to the left of a letter.
  const Selection near(selectObject(page, QPoint(626, 1150), params));
  BOOST_REQUIRE(!near.isNull());
  BOOST_CHECK(near.rect().contains(QPoint(630, 1150)));

  // The middle of nowhere.
  BOOST_CHECK(selectObject(page, QPoint(200, 900), params).isNull());
  // Off the image.
  BOOST_CHECK(selectObject(page, QPoint(-5, 900), params).isNull());
}

BOOST_AUTO_TEST_CASE(tolerance_decides_what_counts_as_ink) {
  QImage page(400, 400, QImage::Format_RGB32);
  page.fill(PAPER);
  // A faint pencil mark: 40 levels darker than the paper.
  for (int y = 100; y < 120; ++y) {
    for (int x = 100; x < 200; ++x) {
      page.setPixel(x, y, qRgb(198, 192, 178));
    }
  }
  ObjectSelectionParams params;
  params.tolerance = 60;
  BOOST_CHECK(selectObject(page, QPoint(150, 110), params).isNull());
  params.tolerance = 30;
  BOOST_CHECK(!selectObject(page, QPoint(150, 110), params).isNull());
}

BOOST_AUTO_TEST_CASE(objects_larger_than_the_first_window_are_taken_whole) {
  // A single bar 6000 px long: much longer than the window the search starts with.
  QImage page(6400, 300, QImage::Format_Grayscale8);
  page.fill(240);
  for (int y = 140; y < 160; ++y) {
    uchar* line = page.scanLine(y);
    for (int x = 100; x < 6100; ++x) {
      line[x] = 10;
    }
  }
  ObjectSelectionParams params;
  params.margin = 0;
  params.grouping = 0;
  const Selection bar(selectObject(page, QPoint(3200, 150), params));
  BOOST_REQUIRE(!bar.isNull());
  BOOST_CHECK(bar.rect() == QRect(100, 140, 6000, 20));
}

BOOST_AUTO_TEST_CASE(edits_carry_over_to_a_scaled_copy) {
  // The same page at half the resolution: what was painted on the full-size
  // scan must cover the same things on the small one, and not less.
  const Edit rect(Edit::rect(QRect(101, 51, 20, 10), Qt::white));
  const Edit halfRect(rect.scaled(0.5, 0.5));
  BOOST_CHECK(halfRect.boundingRect() == QRect(50, 25, 11, 6));
  BOOST_CHECK(rect.scaled(1.0, 1.0).boundingRect() == rect.boundingRect());

  const Edit stroke(Edit::stroke(QPolygonF() << QPointF(10, 10) << QPointF(110, 10), 20, Qt::white));
  const Edit doubled(stroke.scaled(2.0, 2.0));
  BOOST_CHECK(doubled.points()[1] == QPointF(220, 20));
  BOOST_CHECK_GE(doubled.diameter(), 40.0);

  BinaryImage mask(QSize(10, 10), WHITE);
  mask.setPixel(3, 4, BLACK);
  const Edit masked(Edit::mask(mask, QPoint(21, 31), Qt::white));
  const Edit shrunk(masked.scaled(0.5, 0.5));
  const QRect shrunkBox(shrunk.boundingRect());
  const BinaryImage shrunkCover(shrunk.coverage(shrunkBox));
  // Source pixel (24, 35) falls on target pixel (12, 17).
  BOOST_CHECK_EQUAL(shrunkCover.countBlackPixels(), 1);
  BOOST_CHECK(shrunkCover.getPixel(12 - shrunkBox.left(), 17 - shrunkBox.top()) == BLACK);
  const Edit grown(masked.scaled(2.0, 2.0));
  BOOST_CHECK_EQUAL(grown.coverage(grown.boundingRect()).countBlackPixels(), 4);

  // A colour taken from the paper is taken again from the other copy's paper.
  QImage cream(200, 200, QImage::Format_RGB32);
  cream.fill(qRgb(235, 225, 200));
  QImage white(100, 100, QImage::Format_RGB32);
  white.fill(qRgb(255, 255, 255));
  const Edit painted(Edit::rect(QRect(80, 80, 20, 20), QColor(235, 225, 200)).withColor(QColor(235, 225, 200), true));
  const Edit fixed(Edit::rect(QRect(10, 10, 20, 20), Qt::red));
  const std::vector<Edit> carried(carryOver({painted, fixed}, 0.5, 0.5, white, 4));
  BOOST_REQUIRE_EQUAL(carried.size(), size_t(2));
  BOOST_CHECK(carried[0].color() == QColor(255, 255, 255));
  BOOST_CHECK(carried[0].boundingRect() == QRect(40, 40, 10, 10));
  BOOST_CHECK(carried[1].color() == QColor(Qt::red));
}

BOOST_AUTO_TEST_CASE(selections_add_and_subtract) {
  BinaryImage a(QSize(10, 10), BLACK);
  BinaryImage b(QSize(10, 10), BLACK);
  const Selection first(a, QPoint(0, 0));
  const Selection second(b, QPoint(20, 0));
  const Selection both(first.united(second));
  BOOST_CHECK(both.rect() == QRect(0, 0, 30, 10));
  BOOST_CHECK_EQUAL(both.mask().countBlackPixels(), 200);

  const Selection left(both.subtracted(second));
  BOOST_CHECK(left.rect() == QRect(0, 0, 10, 10));
  BOOST_CHECK(both.subtracted(both).isNull());
  // A selection is trimmed to its pixels.
  BinaryImage sparse(QSize(50, 50), WHITE);
  sparse.setPixel(30, 40, BLACK);
  const Selection point(sparse, QPoint(5, 5));
  BOOST_CHECK(point.rect() == QRect(35, 45, 1, 1));
}

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
