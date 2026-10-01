// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <Edit.h>
#include <ImageId.h>
#include <OriginalsBackup.h>
#include <SourceFile.h>
#include <TiffReader.h>
#include <tiffio.h>

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryDir>
#include <boost/test/unit_test.hpp>
#include <csetjmp>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

using namespace retouch;

namespace Tests {
namespace {
/*================================ TIFF ================================*/

struct TestTiff {
  int width = 96;
  int height = 64;
  uint16_t bitsPerSample = 8;
  uint16_t samplesPerPixel = 1;
  uint16_t photometric = PHOTOMETRIC_MINISBLACK;
  uint16_t compression = COMPRESSION_NONE;
  uint16_t predictor = PREDICTOR_NONE;
  std::vector<uint16_t> colormap[3];
  QByteArray icc;
  QByteArray software = "Book Scanner 2.1";
  float dpi = 400;
  int directories = 1;
  bool bigEndian = false;
  uint16_t extraSamples = 0;
};

using LineFiller = std::function<void(int y, std::vector<uint8_t>& line)>;

TIFF* openTiff(const QString& path, const char* mode) {
#ifdef _WIN32
  return TIFFOpenW(path.toStdWString().c_str(), mode);
#else
  return TIFFOpen(QFile::encodeName(path).constData(), mode);
#endif
}

void writeTestTiff(const QString& path, const TestTiff& spec, const LineFiller& fill) {
  TIFF* tif = openTiff(path, spec.bigEndian ? "wb" : "wl");
  BOOST_REQUIRE(tif);
  for (int dir = 0; dir < spec.directories; ++dir) {
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t(spec.width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t(spec.height));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, spec.bitsPerSample);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spec.samplesPerPixel);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, spec.photometric);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, spec.compression);
    if (spec.extraSamples) {
      const uint16_t type = EXTRASAMPLE_UNASSALPHA;
      TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, uint16_t(1), &type);
    }
    if (spec.predictor != PREDICTOR_NONE) {
      TIFFSetField(tif, TIFFTAG_PREDICTOR, spec.predictor);
    }
    if (spec.compression == COMPRESSION_JPEG) {
      TIFFSetField(tif, TIFFTAG_JPEGQUALITY, 90);
      if (spec.photometric == PHOTOMETRIC_YCBCR) {
        TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
      }
    }
    if (spec.photometric == PHOTOMETRIC_PALETTE) {
      TIFFSetField(tif, TIFFTAG_COLORMAP, spec.colormap[0].data(), spec.colormap[1].data(), spec.colormap[2].data());
    }
    TIFFSetField(tif, TIFFTAG_XRESOLUTION, double(spec.dpi));
    TIFFSetField(tif, TIFFTAG_YRESOLUTION, double(spec.dpi));
    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    if (!spec.software.isEmpty()) {
      TIFFSetField(tif, TIFFTAG_SOFTWARE, spec.software.constData());
    }
    if (!spec.icc.isEmpty()) {
      TIFFSetField(tif, TIFFTAG_ICCPROFILE, uint32_t(spec.icc.size()), spec.icc.constData());
    }
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));

    const size_t lineBytes = size_t(TIFFScanlineSize(tif));
    std::vector<uint8_t> line(lineBytes);
    for (int y = 0; y < spec.height; ++y) {
      std::fill(line.begin(), line.end(), 0);
      fill(y, line);
      BOOST_REQUIRE(TIFFWriteScanline(tif, line.data(), uint32_t(y), 0) >= 0);
    }
    BOOST_REQUIRE(TIFFWriteDirectory(tif));
  }
  TIFFClose(tif);
}

struct TiffTags {
  uint16_t bitsPerSample = 0;
  uint16_t samplesPerPixel = 0;
  uint16_t photometric = 0;
  uint16_t compression = 0;
  uint16_t predictor = 0;
  float xres = 0;
  uint16_t resUnit = 0;
  QByteArray software;
  QByteArray icc;
  bool bigEndian = false;
  std::vector<uint16_t> colormap[3];
  uint16_t subsampling[2] = {0, 0};
};

TiffTags readTags(const QString& path) {
  TiffTags tags;
  TIFF* tif = openTiff(path, "r");
  BOOST_REQUIRE(tif);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &tags.bitsPerSample);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &tags.samplesPerPixel);
  TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &tags.photometric);
  TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &tags.compression);
  if ((tags.compression == COMPRESSION_LZW) || (tags.compression == COMPRESSION_ADOBE_DEFLATE)) {
    TIFFGetField(tif, TIFFTAG_PREDICTOR, &tags.predictor);
  }
  TIFFGetField(tif, TIFFTAG_XRESOLUTION, &tags.xres);
  TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &tags.resUnit);
  char* software = nullptr;
  if (TIFFGetField(tif, TIFFTAG_SOFTWARE, &software) && software) {
    tags.software = software;
  }
  uint32_t iccSize = 0;
  void* icc = nullptr;
  if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &iccSize, &icc) && icc) {
    tags.icc = QByteArray(static_cast<const char*>(icc), int(iccSize));
  }
  tags.bigEndian = TIFFIsBigEndian(tif) != 0;
  if (tags.photometric == PHOTOMETRIC_PALETTE) {
    uint16_t* r = nullptr;
    uint16_t* g = nullptr;
    uint16_t* b = nullptr;
    TIFFGetField(tif, TIFFTAG_COLORMAP, &r, &g, &b);
    const size_t n = size_t(1) << tags.bitsPerSample;
    tags.colormap[0].assign(r, r + n);
    tags.colormap[1].assign(g, g + n);
    tags.colormap[2].assign(b, b + n);
  }
  if (tags.photometric == PHOTOMETRIC_YCBCR) {
    TIFFGetFieldDefaulted(tif, TIFFTAG_YCBCRSUBSAMPLING, &tags.subsampling[0], &tags.subsampling[1]);
  }
  TIFFClose(tif);
  return tags;
}

QImage readTiffImage(const QString& path) {
  QFile file(path);
  BOOST_REQUIRE(file.open(QIODevice::ReadOnly));
  return TiffReader::readImage(file, 0);
}

/** Whether \p a and \p b agree on every pixel outside \p except. */
bool sameOutside(const QImage& a, const QImage& b, const QRect& except) {
  if (a.size() != b.size()) {
    return false;
  }
  for (int y = 0; y < a.height(); ++y) {
    for (int x = 0; x < a.width(); ++x) {
      if (!except.contains(x, y) && (a.pixel(x, y) != b.pixel(x, y))) {
        return false;
      }
    }
  }
  return true;
}

bool allPixels(const QImage& image, const QRect& area, const QRgb rgb) {
  for (int y = area.top(); y <= area.bottom(); ++y) {
    for (int x = area.left(); x <= area.right(); ++x) {
      if (image.pixel(x, y) != rgb) {
        return false;
      }
    }
  }
  return true;
}

QStringList filesIn(const QString& dir) {
  return QDir(dir).entryList(QDir::Files, QDir::Name);
}

/*================================ JPEG ================================*/

struct JpegCoefficients {
  int components = 0;
  int maxH = 1;
  int maxV = 1;
  std::vector<int> hSamp, vSamp, widthInBlocks, heightInBlocks;
  std::vector<std::vector<JCOEF>> blocks;  // [component][(by * widthInBlocks + bx) * 64 + k]
};

struct TestJpegError {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void testJpegErrorExit(j_common_ptr cinfo) {
  longjmp(reinterpret_cast<TestJpegError*>(cinfo->err)->jump, 1);
}

/** Reads the quantized DCT coefficients of every block of \p data. */
bool readCoefficients(const QByteArray& data, JpegCoefficients& out) {
  auto err = std::make_unique<TestJpegError>();
  auto info = std::make_unique<jpeg_decompress_struct>();
  info->err = jpeg_std_error(&err->pub);
  err->pub.error_exit = &testJpegErrorExit;
  if (setjmp(err->jump)) {
    jpeg_destroy_decompress(info.get());
    return false;
  }
  jpeg_create_decompress(info.get());
  jpeg_mem_src(info.get(), reinterpret_cast<unsigned char*>(const_cast<char*>(data.constData())),
               static_cast<unsigned long>(data.size()));
  jpeg_read_header(info.get(), TRUE);
  jvirt_barray_ptr* arrays = jpeg_read_coefficients(info.get());
  out.components = info->num_components;
  out.maxH = info->max_h_samp_factor;
  out.maxV = info->max_v_samp_factor;
  out.blocks.resize(size_t(out.components));
  for (int ci = 0; ci < out.components; ++ci) {
    const jpeg_component_info& comp = info->comp_info[ci];
    out.hSamp.push_back(comp.h_samp_factor);
    out.vSamp.push_back(comp.v_samp_factor);
    out.widthInBlocks.push_back(int(comp.width_in_blocks));
    out.heightInBlocks.push_back(int(comp.height_in_blocks));
    std::vector<JCOEF>& blocks = out.blocks[size_t(ci)];
    blocks.resize(size_t(comp.width_in_blocks) * comp.height_in_blocks * 64);
    for (JDIMENSION by = 0; by < comp.height_in_blocks; ++by) {
      JBLOCKARRAY row
          = (*info->mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(info.get()), arrays[ci], by, 1, FALSE);
      for (JDIMENSION bx = 0; bx < comp.width_in_blocks; ++bx) {
        std::copy(row[0][bx], row[0][bx] + 64, &blocks[(size_t(by) * comp.width_in_blocks + bx) * 64]);
      }
    }
  }
  jpeg_finish_decompress(info.get());
  jpeg_destroy_decompress(info.get());
  return true;
}

/** A photograph-like test pattern: smooth colour gradients with fine detail on top. */
QImage photoPattern(const int width, const int height) {
  QImage image(width, height, QImage::Format_RGB32);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int r = (x * 255) / width;
      const int g = (y * 255) / height;
      const int b = ((x ^ y) & 0x1f) * 4 + 60;
      image.setPixel(x, y, qRgb(r, g, b));
    }
  }
  return image;
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QByteArray();
  }
  return file.readAll();
}
}  // namespace

BOOST_AUTO_TEST_SUITE(RetouchFilesTestSuite)

BOOST_AUTO_TEST_CASE(bilevel_g4_tiff_keeps_its_format_and_tags) {
  QTemporaryDir dir;
  BOOST_REQUIRE(dir.isValid());
  const QString path(dir.filePath("bilevel.tif"));
  TestTiff spec;
  spec.bitsPerSample = 1;
  spec.photometric = PHOTOMETRIC_MINISWHITE;
  spec.compression = COMPRESSION_CCITTFAX4;
  spec.dpi = 600;
  spec.icc = QByteArray("not really an ICC profile, but bytes all the same");
  // Black vertical stripes, 8 px wide every 16 px: in MINISWHITE, 1 is black.
  writeTestTiff(path, spec, [&](int, std::vector<uint8_t>& line) {
    for (size_t i = 0; i < line.size(); i += 2) {
      line[i] = 0xff;
    }
  });
  const QImage before(readTiffImage(path));
  BOOST_REQUIRE(!before.isNull());
  BOOST_REQUIRE(SourceFile::unsupportedReason(ImageId(path)).isEmpty());

  const QRect area(10, 5, 30, 20);
  QImage edited;
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(area, QColor(250, 248, 240))}, &edited, &error),
                        error.toStdString());

  const TiffTags tags(readTags(path));
  BOOST_CHECK_EQUAL(tags.bitsPerSample, 1);
  BOOST_CHECK_EQUAL(tags.photometric, PHOTOMETRIC_MINISWHITE);
  BOOST_CHECK_EQUAL(tags.compression, COMPRESSION_CCITTFAX4);
  BOOST_CHECK_CLOSE(tags.xres, 600.0f, 0.01);
  BOOST_CHECK_EQUAL(tags.resUnit, RESUNIT_INCH);
  BOOST_CHECK(tags.software == spec.software);
  BOOST_CHECK(tags.icc == spec.icc);

  const QImage after(readTiffImage(path));
  BOOST_CHECK(after.format() == QImage::Format_Mono);
  BOOST_CHECK(allPixels(after, area, qRgb(255, 255, 255)));
  BOOST_CHECK(sameOutside(before, after, area));
  BOOST_CHECK(sameOutside(edited, after, QRect()));
  // Nothing left behind next to it.
  BOOST_CHECK(filesIn(dir.path()) == QStringList("bilevel.tif"));
}

BOOST_AUTO_TEST_CASE(gray_lzw_tiff_keeps_its_predictor) {
  QTemporaryDir dir;
  const QString path(dir.filePath("gray.tif"));
  TestTiff spec;
  spec.compression = COMPRESSION_LZW;
  spec.predictor = PREDICTOR_HORIZONTAL;
  writeTestTiff(path, spec, [&](int y, std::vector<uint8_t>& line) {
    for (size_t x = 0; x < line.size(); ++x) {
      line[x] = uint8_t((x * 3 + y * 5) & 0xff);
    }
  });
  const QImage before(readTiffImage(path));

  const QRect area(20, 20, 10, 10);
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(area, QColor(200, 180, 160))}, nullptr, &error),
                        error.toStdString());

  const TiffTags tags(readTags(path));
  BOOST_CHECK_EQUAL(tags.compression, COMPRESSION_LZW);
  BOOST_CHECK_EQUAL(tags.predictor, PREDICTOR_HORIZONTAL);
  BOOST_CHECK_EQUAL(tags.photometric, PHOTOMETRIC_MINISBLACK);
  const QImage after(readTiffImage(path));
  const int gray = qGray(qRgb(200, 180, 160));
  BOOST_CHECK(allPixels(after, area, qRgb(gray, gray, gray)));
  BOOST_CHECK(sameOutside(before, after, area));
}

BOOST_AUTO_TEST_CASE(palette_tiff_keeps_its_palette) {
  QTemporaryDir dir;
  const QString path(dir.filePath("palette.tif"));
  TestTiff spec;
  spec.photometric = PHOTOMETRIC_PALETTE;
  spec.compression = COMPRESSION_PACKBITS;
  for (int i = 0; i < 256; ++i) {
    spec.colormap[0].push_back(uint16_t((i * 7 % 256) * 257));
    spec.colormap[1].push_back(uint16_t((i * 13 % 256) * 257));
    spec.colormap[2].push_back(uint16_t((255 - i) * 257));
  }
  // One white entry, for a white fill to map to.
  spec.colormap[0][200] = spec.colormap[1][200] = spec.colormap[2][200] = 0xffff;
  writeTestTiff(path, spec, [&](int y, std::vector<uint8_t>& line) {
    for (size_t x = 0; x < line.size(); ++x) {
      line[x] = uint8_t((x + y) % 100);
    }
  });
  const QImage before(readTiffImage(path));

  const QRect area(0, 0, 8, 8);
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(area, Qt::white)}, nullptr, &error),
                        error.toStdString());

  const TiffTags tags(readTags(path));
  BOOST_CHECK_EQUAL(tags.photometric, PHOTOMETRIC_PALETTE);
  BOOST_CHECK_EQUAL(tags.compression, COMPRESSION_PACKBITS);
  BOOST_CHECK(tags.colormap[0] == spec.colormap[0]);
  BOOST_CHECK(tags.colormap[1] == spec.colormap[1]);
  BOOST_CHECK(tags.colormap[2] == spec.colormap[2]);
  const QImage after(readTiffImage(path));
  BOOST_CHECK_EQUAL(after.pixelIndex(3, 3), 200);
  BOOST_CHECK(sameOutside(before, after, area));
}

BOOST_AUTO_TEST_CASE(big_endian_rgb_tiff_stays_big_endian) {
  if (!TIFFIsCODECConfigured(COMPRESSION_ADOBE_DEFLATE)) {
    BOOST_TEST_MESSAGE("libtiff was built without Deflate; skipped");
    return;
  }
  QTemporaryDir dir;
  const QString path(dir.filePath("rgb.tif"));
  TestTiff spec;
  spec.samplesPerPixel = 3;
  spec.photometric = PHOTOMETRIC_RGB;
  spec.compression = COMPRESSION_ADOBE_DEFLATE;
  spec.predictor = PREDICTOR_HORIZONTAL;
  spec.bigEndian = true;
  writeTestTiff(path, spec, [&](int y, std::vector<uint8_t>& line) {
    for (size_t x = 0; x < line.size(); ++x) {
      line[x] = uint8_t((x * 11 + y * 3) & 0xff);
    }
  });
  const QImage before(readTiffImage(path));

  const QPolygonF stroke(QPolygonF() << QPointF(10.5, 10.5) << QPointF(60.5, 40.5));
  const Edit edit(Edit::stroke(stroke, 6, QColor(12, 34, 56)));
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {edit}, nullptr, &error), error.toStdString());

  const TiffTags tags(readTags(path));
  BOOST_CHECK(tags.bigEndian);
  BOOST_CHECK_EQUAL(tags.compression, COMPRESSION_ADOBE_DEFLATE);
  BOOST_CHECK_EQUAL(tags.predictor, PREDICTOR_HORIZONTAL);
  BOOST_CHECK_EQUAL(tags.samplesPerPixel, 3);

  QImage expected(before);
  applyEdits(expected, {edit});
  const QImage after(readTiffImage(path));
  BOOST_CHECK(sameOutside(expected, after, QRect()));
  BOOST_CHECK(after.pixel(35, 25) == qRgb(12, 34, 56));
}

BOOST_AUTO_TEST_CASE(jpeg_compressed_tiff_is_reencoded_closely) {
  if (!TIFFIsCODECConfigured(COMPRESSION_JPEG)) {
    BOOST_TEST_MESSAGE("libtiff was built without JPEG; skipped");
    return;
  }
  QTemporaryDir dir;
  const QString path(dir.filePath("ycbcr.tif"));
  TestTiff spec;
  spec.width = 128;
  spec.height = 96;
  spec.samplesPerPixel = 3;
  spec.photometric = PHOTOMETRIC_YCBCR;
  spec.compression = COMPRESSION_JPEG;
  writeTestTiff(path, spec, [&](int y, std::vector<uint8_t>& line) {
    for (int x = 0; x < spec.width; ++x) {
      line[size_t(x) * 3] = uint8_t(x * 2);
      line[size_t(x) * 3 + 1] = uint8_t(y * 2);
      line[size_t(x) * 3 + 2] = 128;
    }
  });
  const QImage before(readTiffImage(path));
  BOOST_REQUIRE(!before.isNull());

  const QRect area(32, 32, 32, 32);
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(area, Qt::white)}, nullptr, &error),
                        error.toStdString());

  const TiffTags tags(readTags(path));
  BOOST_CHECK_EQUAL(tags.compression, COMPRESSION_JPEG);
  BOOST_CHECK_EQUAL(tags.photometric, PHOTOMETRIC_YCBCR);
  BOOST_CHECK_EQUAL(tags.subsampling[0], 2);
  BOOST_CHECK_EQUAL(tags.subsampling[1], 2);

  const QImage after(readTiffImage(path));
  // Inside the patch, white give or take the codec.
  for (int y = area.top() + 2; y <= area.bottom() - 2; ++y) {
    for (int x = area.left() + 2; x <= area.right() - 2; ++x) {
      BOOST_REQUIRE_GE(qGray(after.pixel(x, y)), 245);
    }
  }
  // Elsewhere, within a few levels of what it was.
  double totalDiff = 0;
  int count = 0;
  for (int y = 0; y < before.height(); ++y) {
    for (int x = 0; x < before.width(); ++x) {
      if (!area.adjusted(-8, -8, 8, 8).contains(x, y)) {
        totalDiff += std::abs(qGray(before.pixel(x, y)) - qGray(after.pixel(x, y)));
        ++count;
      }
    }
  }
  BOOST_CHECK_LT(totalDiff / count, 3.0);
}

BOOST_AUTO_TEST_CASE(tiffs_that_would_lose_data_are_refused) {
  QTemporaryDir dir;
  const LineFiller zero = [](int, std::vector<uint8_t>&) {};

  TestTiff deep;
  deep.bitsPerSample = 16;
  writeTestTiff(dir.filePath("deep.tif"), deep, zero);
  BOOST_CHECK(!SourceFile::unsupportedReason(ImageId(dir.filePath("deep.tif"))).isEmpty());

  TestTiff multi;
  multi.directories = 2;
  writeTestTiff(dir.filePath("multi.tif"), multi, zero);
  const QString reason(SourceFile::unsupportedReason(ImageId(dir.filePath("multi.tif"))));
  BOOST_CHECK(reason.contains("2"));

  TestTiff alpha;
  alpha.samplesPerPixel = 4;
  alpha.photometric = PHOTOMETRIC_RGB;
  alpha.extraSamples = 1;
  writeTestTiff(dir.filePath("alpha.tif"), alpha, zero);
  BOOST_CHECK(!SourceFile::unsupportedReason(ImageId(dir.filePath("alpha.tif"))).isEmpty());

  // And a refused file is never touched.
  const QByteArray before(readFile(dir.filePath("deep.tif")));
  QString error;
  BOOST_CHECK(!SourceFile::rewrite(ImageId(dir.filePath("deep.tif")), {Edit::rect(QRect(0, 0, 4, 4), Qt::white)},
                                   nullptr, &error));
  BOOST_CHECK(!error.isEmpty());
  BOOST_CHECK(readFile(dir.filePath("deep.tif")) == before);

  BOOST_CHECK(!SourceFile::unsupportedReason(ImageId(dir.filePath("missing.tif"))).isEmpty());
}

BOOST_AUTO_TEST_CASE(jpeg_patch_keeps_untouched_blocks_and_markers) {
  QTemporaryDir dir;
  const QString path(dir.filePath("photo.jpg"));
  QImage source(photoPattern(256, 192));
  source.setDotsPerMeterX(11811);  // 300 dpi
  source.setDotsPerMeterY(11811);
  source.setText("Description", "page 42");
  {
    QImageWriter writer(path, "jpg");
    writer.setQuality(85);
    BOOST_REQUIRE(writer.write(source));
  }
  BOOST_REQUIRE(SourceFile::unsupportedReason(ImageId(path)).isEmpty());
  const QByteArray before(readFile(path));
  JpegCoefficients original;
  BOOST_REQUIRE(readCoefficients(before, original));

  const QRect area(70, 50, 40, 20);
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(area, Qt::white)}, nullptr, &error),
                        error.toStdString());
  const QByteArray after(readFile(path));
  BOOST_CHECK(after != before);

  JpegCoefficients patched;
  BOOST_REQUIRE(readCoefficients(after, patched));
  BOOST_REQUIRE_EQUAL(patched.components, original.components);

  // Every block whose pixels the edit does not reach is exactly as it was.
  int untouched = 0;
  int changed = 0;
  for (int ci = 0; ci < original.components; ++ci) {
    BOOST_REQUIRE_EQUAL(patched.widthInBlocks[ci], original.widthInBlocks[ci]);
    const int sx = original.maxH / original.hSamp[ci];
    const int sy = original.maxV / original.vSamp[ci];
    for (int by = 0; by < original.heightInBlocks[ci]; ++by) {
      for (int bx = 0; bx < original.widthInBlocks[ci]; ++bx) {
        const QRect pixels(bx * 8 * sx, by * 8 * sy, 8 * sx, 8 * sy);
        const size_t offset = (size_t(by) * original.widthInBlocks[ci] + bx) * 64;
        const bool same
            = std::equal(&original.blocks[ci][offset], &original.blocks[ci][offset] + 64, &patched.blocks[ci][offset]);
        if (!pixels.intersects(area)) {
          BOOST_CHECK(same);
          ++untouched;
        } else if (!same) {
          ++changed;
        }
      }
    }
  }
  BOOST_CHECK_GT(untouched, 0);
  BOOST_CHECK_GT(changed, 0);

  // The edit is there.
  QImageReader reader(path);
  reader.setAutoTransform(false);
  const QImage decoded(reader.read());
  for (int y = area.top() + 1; y < area.bottom(); ++y) {
    for (int x = area.left() + 1; x < area.right(); ++x) {
      BOOST_REQUIRE_GE(qGray(decoded.pixel(x, y)), 240);
    }
  }
  // And so are the comment and the resolution.
  BOOST_CHECK(reader.text("Description") == QString("page 42"));
  BOOST_CHECK_EQUAL(qRound(decoded.dotsPerMeterX() * 0.0254), 300);
}

BOOST_AUTO_TEST_CASE(grayscale_jpeg_is_patched_too) {
  QTemporaryDir dir;
  const QString path(dir.filePath("gray.jpg"));
  const QImage source(photoPattern(128, 128).convertToFormat(QImage::Format_Grayscale8));
  BOOST_REQUIRE(source.save(path, "jpg", 90));
  const QRect area(16, 16, 16, 16);
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(area, Qt::black)}, nullptr, &error),
                        error.toStdString());
  const QImage decoded(path);
  BOOST_CHECK(decoded.isGrayscale());
  for (int y = area.top() + 1; y < area.bottom(); ++y) {
    for (int x = area.left() + 1; x < area.right(); ++x) {
      BOOST_REQUIRE_LE(qGray(decoded.pixel(x, y)), 12);
    }
  }
}

BOOST_AUTO_TEST_CASE(png_keeps_palette_resolution_and_text) {
  QTemporaryDir dir;
  const QString path(dir.filePath("indexed.png"));
  QImage source(64, 64, QImage::Format_Indexed8);
  source.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0), qRgb(200, 30, 30)});
  source.fill(1);
  source.setDotsPerMeterX(15748);  // 400 dpi
  source.setDotsPerMeterY(15748);
  source.setText("Source", "scanner 3");
  BOOST_REQUIRE(source.save(path, "png"));

  const QRect area(10, 10, 20, 20);
  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(area, QColor(250, 250, 250))}, nullptr, &error),
                        error.toStdString());
  QImageReader reader(path);
  const QImage after(reader.read());
  BOOST_CHECK(after.format() == QImage::Format_Indexed8);
  BOOST_CHECK_EQUAL(after.colorCount(), 3);
  BOOST_CHECK_EQUAL(after.pixelIndex(15, 15), 0);
  BOOST_CHECK_EQUAL(after.pixelIndex(40, 40), 1);
  BOOST_CHECK_EQUAL(after.dotsPerMeterX(), 15748);
  BOOST_CHECK(after.text("Source") == QString("scanner 3"));
}

BOOST_AUTO_TEST_CASE(backup_keeps_the_first_original_and_restores_it) {
  QTemporaryDir dir;
  QDir(dir.path()).mkpath("in");
  QDir(dir.path()).mkpath("out");
  const QString source(dir.filePath("in/page.tif"));
  {
    QFile file(source);
    BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("original scan");
  }

  OriginalsBackup backup(dir.filePath("out"));
  BOOST_CHECK(!backup.contains(source));
  QString error;
  BOOST_REQUIRE_MESSAGE(backup.keep(source, &error), error.toStdString());
  BOOST_CHECK(backup.contains(source));
  BOOST_CHECK(readFile(backup.pathFor(source)) == "original scan");
  BOOST_CHECK(QFileInfo(backup.pathFor(source)).fileName().startsWith("page."));
  BOOST_CHECK(QFileInfo(backup.pathFor(source)).fileName().endsWith(".tif"));

  // A second retouch must not replace the original with a retouched version.
  {
    QFile file(source);
    BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("retouched once");
  }
  BOOST_REQUIRE(backup.keep(source, &error));
  BOOST_CHECK(readFile(backup.pathFor(source)) == "original scan");

  // Same name, other folder: kept separately.
  QDir(dir.path()).mkpath("in2");
  const QString namesake(dir.filePath("in2/page.tif"));
  {
    QFile file(namesake);
    BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("another scan");
  }
  BOOST_CHECK(backup.pathFor(namesake) != backup.pathFor(source));

  BOOST_REQUIRE_MESSAGE(backup.restore(source, &error), error.toStdString());
  BOOST_CHECK(readFile(source) == "original scan");
  // Restored in place: nothing new beside the scan, and the copy stays - it
  // is still the original, and nothing here may count on deleting a file.
  BOOST_CHECK(filesIn(dir.filePath("in")) == QStringList("page.tif"));
  BOOST_CHECK(backup.contains(source));
  BOOST_CHECK(readFile(backup.directory() + "/index.txt").contains("restored"));
  // Retouched and restored again, it is the same original that comes back.
  {
    QFile file(source);
    BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("retouched twice");
  }
  BOOST_REQUIRE(backup.keep(source, &error));
  BOOST_REQUIRE(backup.restore(source, &error));
  BOOST_CHECK(readFile(source) == "original scan");
  BOOST_CHECK(!backup.restore(namesake, &error));
}

BOOST_AUTO_TEST_CASE(backup_cut_short_is_made_again) {
  QTemporaryDir dir;
  const QString source(dir.filePath("page.tif"));
  {
    QFile file(source);
    BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("the whole original scan");
  }
  OriginalsBackup backup(dir.filePath("out"));
  // What an interrupted copy leaves: part of the data under the final name,
  // and no mark that it was completed.
  BOOST_REQUIRE(QDir().mkpath(backup.directory()));
  {
    QFile partial(backup.pathFor(source));
    BOOST_REQUIRE(partial.open(QIODevice::WriteOnly));
    partial.write("the whole");
  }
  BOOST_CHECK(!backup.contains(source));

  QString error;
  BOOST_REQUIRE_MESSAGE(backup.keep(source, &error), error.toStdString());
  BOOST_CHECK(backup.contains(source));
  BOOST_CHECK(readFile(backup.pathFor(source)) == "the whole original scan");
}

BOOST_AUTO_TEST_CASE(rewriting_needs_no_rename) {
  // Shares that refuse renaming and deleting must still take a retouch: the
  // file is written in place, and no temporary file is left beside it.
  QTemporaryDir dir;
  const QString path(dir.filePath("plain.png"));
  QImage source(32, 32, QImage::Format_RGB32);
  source.fill(qRgb(10, 10, 10));
  BOOST_REQUIRE(source.save(path, "png"));
  const QDateTime created(QFileInfo(path).birthTime());

  QString error;
  BOOST_REQUIRE_MESSAGE(SourceFile::rewrite(ImageId(path), {Edit::rect(QRect(0, 0, 8, 8), Qt::white)}, nullptr, &error),
                        error.toStdString());
  BOOST_CHECK(filesIn(dir.path()) == QStringList("plain.png"));
  BOOST_CHECK(QImage(path).pixel(2, 2) == qRgb(255, 255, 255));
  // The same file, written over, rather than a new one renamed into place.
  if (created.isValid()) {
    BOOST_CHECK(QFileInfo(path).birthTime() == created);
  }
}

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
