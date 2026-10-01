// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "SourceFile.h"

#include <tiffio.h>

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Diagnostics.h"
#include "FileIo.h"
#include "ImageId.h"
#include "JpegPatch.h"
#include "TiffReader.h"

namespace retouch {
namespace {
enum class FileFormat { UNKNOWN, TIFF, JPEG, PNG, BMP };

FileFormat detectFormat(QIODevice& device) {
  if (TiffReader::canRead(device)) {
    return FileFormat::TIFF;
  }
  const QByteArray head(device.peek(8));
  if (head.startsWith("\xFF\xD8\xFF")) {
    return FileFormat::JPEG;
  }
  if (head.startsWith("\x89PNG\r\n\x1A\n")) {
    return FileFormat::PNG;
  }
  if (head.startsWith("BM")) {
    return FileFormat::BMP;
  }
  return FileFormat::UNKNOWN;
}

/*============================== TIFF ==============================*/

tsize_t deviceRead(thandle_t context, tdata_t data, tsize_t size) {
  return static_cast<tsize_t>(static_cast<QIODevice*>(context)->read(static_cast<char*>(data), size));
}

tsize_t deviceWrite(thandle_t context, tdata_t data, tsize_t size) {
  return static_cast<tsize_t>(static_cast<QIODevice*>(context)->write(static_cast<const char*>(data), size));
}

toff_t deviceSeek(thandle_t context, toff_t offset, int whence) {
  auto* device = static_cast<QIODevice*>(context);
  switch (whence) {
    case SEEK_SET:
      device->seek(static_cast<qint64>(offset));
      break;
    case SEEK_CUR:
      device->seek(device->pos() + static_cast<qint64>(offset));
      break;
    case SEEK_END:
      device->seek(device->size() + static_cast<qint64>(offset));
      break;
    default:
      break;
  }
  return static_cast<toff_t>(device->pos());
}

// The device belongs to the caller: the file being read, or the buffer the new
// file is encoded into before it is written out.
int deviceClose(thandle_t) {
  return 0;
}

toff_t deviceSize(thandle_t context) {
  return static_cast<toff_t>(static_cast<QIODevice*>(context)->size());
}

int deviceMap(thandle_t, tdata_t*, toff_t*) {
  return 0;
}

void deviceUnmap(thandle_t, tdata_t, toff_t) {}

#if defined(TIFFLIB_AT_LEAST) && TIFFLIB_AT_LEAST(4, 5, 0)
#define RETOUCH_TIFF_ERROR_CAPTURE 1

/** The first error libtiff reports, which is the one that says what went wrong. */
int captureError(TIFF*, void* userData, const char* module, const char* fmt, va_list ap) {
  auto* message = static_cast<QString*>(userData);
  if (message->isEmpty()) {
    char buffer[512];
    std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    *message = QString::fromLocal8Bit(buffer);
    if (module && *module) {
      *message = QString::fromLocal8Bit(module) + QLatin1String(": ") + *message;
    }
  }
  return 1;
}

int ignoreWarning(TIFF*, void*, const char*, const char*, va_list) {
  return 1;
}
#endif

/** A libtiff handle on a QIODevice, closed when it goes out of scope. */
class TiffHandle {
 public:
  TiffHandle(QIODevice& device, const char* mode) {
#ifdef RETOUCH_TIFF_ERROR_CAPTURE
    TIFFOpenOptions* options = TIFFOpenOptionsAlloc();
    TIFFOpenOptionsSetErrorHandlerExtR(options, &captureError, &m_error);
    TIFFOpenOptionsSetWarningHandlerExtR(options, &ignoreWarning, nullptr);
    m_tif = TIFFClientOpenExt("retouch", mode, &device, &deviceRead, &deviceWrite, &deviceSeek, &deviceClose,
                              &deviceSize, &deviceMap, &deviceUnmap, options);
    TIFFOpenOptionsFree(options);
#else
    m_tif = TIFFClientOpen("retouch", mode, &device, &deviceRead, &deviceWrite, &deviceSeek, &deviceClose, &deviceSize,
                           &deviceMap, &deviceUnmap);
#endif
  }

  ~TiffHandle() {
    if (m_tif) {
      TIFFClose(m_tif);
    }
  }

  TiffHandle(const TiffHandle&) = delete;
  TiffHandle& operator=(const TiffHandle&) = delete;

  TIFF* get() const { return m_tif; }

  /** \brief Closes the file now, which is where libtiff writes what it still holds. */
  void close() {
    if (m_tif) {
      TIFFClose(m_tif);
      m_tif = nullptr;
    }
  }

  /** \brief What libtiff said went wrong, if anything and if it could be captured. */
  const QString& error() const { return m_error; }

 private:
  TIFF* m_tif = nullptr;
  QString m_error;
};

/** The descriptive tags carried over as they are. */
const ttag_t ASCII_TAGS[]
    = {TIFFTAG_DOCUMENTNAME, TIFFTAG_IMAGEDESCRIPTION, TIFFTAG_MAKE,   TIFFTAG_MODEL,        TIFFTAG_PAGENAME,
       TIFFTAG_SOFTWARE,     TIFFTAG_DATETIME,         TIFFTAG_ARTIST, TIFFTAG_HOSTCOMPUTER, TIFFTAG_COPYRIGHT};

#ifdef RETOUCH_TIFF_ERROR_CAPTURE
/** Tags holding a block of data: colour profile, XMP, Photoshop resources, IPTC. */
const ttag_t BLOB_TAGS[] = {TIFFTAG_ICCPROFILE, TIFFTAG_XMLPACKET, TIFFTAG_PHOTOSHOP, TIFFTAG_RICHTIFFIPTC};

/** The size of one unit of a blob tag's count: IPTC is counted in 32-bit words by some versions. */
int blobUnit(TIFF* tif, const ttag_t tag) {
  const TIFFField* field = TIFFFieldWithTag(tif, tag);
  return field ? std::max(1, TIFFFieldSetGetSize(field)) : 1;
}
#else
// Before libtiff 4.5 the unit of IPTC's count cannot be asked for, so IPTC is left out.
const ttag_t BLOB_TAGS[] = {TIFFTAG_ICCPROFILE, TIFFTAG_XMLPACKET, TIFFTAG_PHOTOSHOP};

int blobUnit(TIFF*, ttag_t) {
  return 1;
}
#endif

/** Everything needed to write a page the way the source file has it. */
struct TiffLayout {
  int directories = 0;
  bool bigEndian = false;
  bool bigTiff = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t bitsPerSample = 1;
  uint16_t samplesPerPixel = 1;
  bool hasSampleFormat = false;
  uint16_t sampleFormat = SAMPLEFORMAT_UINT;
  uint16_t photometric = PHOTOMETRIC_MINISBLACK;
  uint16_t compression = COMPRESSION_NONE;
  bool hasPredictor = false;
  uint16_t predictor = PREDICTOR_NONE;
  bool tiled = false;
  uint32_t rowsPerStrip = 0;
  bool hasOrientation = false;
  uint16_t orientation = ORIENTATION_TOPLEFT;
  bool hasResolution = false;
  float xResolution = 0;
  float yResolution = 0;
  uint16_t resolutionUnit = RESUNIT_INCH;
  std::vector<uint16_t> colormap[3];
  bool hasT4Options = false;
  uint32_t t4Options = 0;
  bool hasT6Options = false;
  uint32_t t6Options = 0;
  uint16_t ycbcrSubsampling[2] = {2, 2};
  bool hasPageNumber = false;
  uint16_t pageNumber[2] = {0, 0};
  bool hasSubfileType = false;
  uint32_t subfileType = 0;
  bool hasPosition = false;
  float xPosition = 0;
  float yPosition = 0;
  std::vector<std::pair<ttag_t, QByteArray>> asciiTags;
  std::vector<std::pair<ttag_t, QByteArray>> blobTags;

  /** Whether TiffReader decodes this page through libtiff's RGBA interface rather than line by line. */
  bool decodedAsRgba() const { return samplesPerPixel != 1; }
};

bool usesPredictor(const uint16_t compression) {
  switch (compression) {
    case COMPRESSION_LZW:
    case COMPRESSION_ADOBE_DEFLATE:
    case COMPRESSION_DEFLATE:
#ifdef COMPRESSION_LZMA
    case COMPRESSION_LZMA:
#endif
#ifdef COMPRESSION_ZSTD
    case COMPRESSION_ZSTD:
#endif
      return true;
    default:
      return false;
  }
}

bool readTiffLayout(QIODevice& device, TiffLayout& layout, QString& error) {
  device.seek(0);
  TiffHandle handle(device, "rm");
  TIFF* const tif = handle.get();
  if (!tif) {
    error = SourceFile::tr("it could not be read as TIFF (%1)").arg(handle.error());
    return false;
  }

  layout.directories = TIFFNumberOfDirectories(tif);
  if (!TIFFSetDirectory(tif, 0)) {
    error = SourceFile::tr("its first image could not be read");
    return false;
  }
  layout.bigEndian = TIFFIsBigEndian(tif) != 0;
  layout.bigTiff = TIFFIsBigTIFF(tif) != 0;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &layout.width);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &layout.height);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &layout.bitsPerSample);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &layout.samplesPerPixel);
  layout.hasSampleFormat = TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &layout.sampleFormat) != 0;
  TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &layout.compression);

  // A file without the tag is read the way TiffReader reads it, and written
  // with the tag spelled out.
  switch (layout.compression) {
    case COMPRESSION_CCITTFAX3:
    case COMPRESSION_CCITTFAX4:
    case COMPRESSION_CCITTRLE:
    case COMPRESSION_CCITTRLEW:
      layout.photometric = PHOTOMETRIC_MINISWHITE;
      break;
    default:
      layout.photometric = PHOTOMETRIC_MINISBLACK;
      break;
  }
  TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &layout.photometric);

  if (usesPredictor(layout.compression)) {
    uint16_t predictor = PREDICTOR_NONE;
    if (TIFFGetField(tif, TIFFTAG_PREDICTOR, &predictor) && (predictor != PREDICTOR_NONE)) {
      layout.hasPredictor = true;
      layout.predictor = predictor;
    }
  }
  if (layout.compression == COMPRESSION_CCITTFAX3) {
    layout.hasT4Options = TIFFGetField(tif, TIFFTAG_T4OPTIONS, &layout.t4Options) != 0;
  } else if (layout.compression == COMPRESSION_CCITTFAX4) {
    layout.hasT6Options = TIFFGetField(tif, TIFFTAG_T6OPTIONS, &layout.t6Options) != 0;
  }

  layout.tiled = TIFFIsTiled(tif) != 0;
  if (!layout.tiled) {
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &layout.rowsPerStrip);
  }
  layout.hasOrientation = TIFFGetField(tif, TIFFTAG_ORIENTATION, &layout.orientation) != 0;

  if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &layout.xResolution)
      && TIFFGetField(tif, TIFFTAG_YRESOLUTION, &layout.yResolution)) {
    layout.hasResolution = true;
    TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &layout.resolutionUnit);
  }

  if (layout.photometric == PHOTOMETRIC_PALETTE) {
    uint16_t* red = nullptr;
    uint16_t* green = nullptr;
    uint16_t* blue = nullptr;
    if (!TIFFGetField(tif, TIFFTAG_COLORMAP, &red, &green, &blue) || !red || !green || !blue
        || (layout.bitsPerSample > 8)) {
      error = SourceFile::tr("its palette could not be read");
      return false;
    }
    const size_t entries = size_t(1) << layout.bitsPerSample;
    layout.colormap[0].assign(red, red + entries);
    layout.colormap[1].assign(green, green + entries);
    layout.colormap[2].assign(blue, blue + entries);
  }
  if (layout.photometric == PHOTOMETRIC_YCBCR) {
    TIFFGetFieldDefaulted(tif, TIFFTAG_YCBCRSUBSAMPLING, &layout.ycbcrSubsampling[0], &layout.ycbcrSubsampling[1]);
  }

  layout.hasPageNumber = TIFFGetField(tif, TIFFTAG_PAGENUMBER, &layout.pageNumber[0], &layout.pageNumber[1]) != 0;
  layout.hasSubfileType = TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &layout.subfileType) != 0;
  layout.hasPosition = TIFFGetField(tif, TIFFTAG_XPOSITION, &layout.xPosition)
                       && TIFFGetField(tif, TIFFTAG_YPOSITION, &layout.yPosition);

  for (const ttag_t tag : ASCII_TAGS) {
    char* text = nullptr;
    if (TIFFGetField(tif, tag, &text) && text) {
      layout.asciiTags.emplace_back(tag, QByteArray(text));
    }
  }
  for (const ttag_t tag : BLOB_TAGS) {
    uint32_t count = 0;
    void* data = nullptr;
    if (TIFFGetField(tif, tag, &count, &data) && data && count) {
      layout.blobTags.emplace_back(tag, QByteArray(static_cast<const char*>(data), int(count) * blobUnit(tif, tag)));
    }
  }
  return true;
}

/** Why a page laid out as \p layout cannot be written back losslessly, or an empty string. */
QString tiffUnsupportedReason(const TiffLayout& layout) {
  if (layout.directories != 1) {
    return SourceFile::tr("it holds %1 images; only TIFF files with a single image can be retouched")
        .arg(layout.directories);
  }
  if (layout.hasSampleFormat && (layout.sampleFormat != SAMPLEFORMAT_UINT)) {
    return SourceFile::tr("its samples are not unsigned integers");
  }
  if (layout.samplesPerPixel == 1) {
    if ((layout.bitsPerSample != 1) && (layout.bitsPerSample != 8)) {
      return SourceFile::tr("it has %1 bits per pixel").arg(layout.bitsPerSample);
    }
    switch (layout.photometric) {
      case PHOTOMETRIC_MINISWHITE:
      case PHOTOMETRIC_MINISBLACK:
      case PHOTOMETRIC_PALETTE:
        break;
      default:
        return SourceFile::tr("its colour space (%1) is not supported").arg(layout.photometric);
    }
  } else if (layout.samplesPerPixel == 3) {
    if (layout.bitsPerSample != 8) {
      return SourceFile::tr("it has %1 bits per channel").arg(layout.bitsPerSample);
    }
    const bool rgb = (layout.photometric == PHOTOMETRIC_RGB);
    const bool jpegYcbcr = (layout.photometric == PHOTOMETRIC_YCBCR) && (layout.compression == COMPRESSION_JPEG);
    if (!rgb && !jpegYcbcr) {
      return SourceFile::tr("its colour space (%1) is not supported").arg(layout.photometric);
    }
    // libtiff's RGBA reader flips the image to the top-left corner, but does not
    // rotate it. Written back, a rotated file would come out turned.
    if (layout.hasOrientation && (layout.orientation > ORIENTATION_BOTLEFT)) {
      return SourceFile::tr("its orientation tag rotates the image");
    }
  } else {
    return SourceFile::tr("it has %1 channels (transparency or CMYK)").arg(layout.samplesPerPixel);
  }
  if (layout.compression == COMPRESSION_OJPEG) {
    return SourceFile::tr("it uses old-style JPEG compression");
  }
  if (!TIFFIsCODECConfigured(layout.compression)) {
    return SourceFile::tr("its compression (%1) cannot be written").arg(layout.compression);
  }
  return QString();
}

bool writeTiff(QIODevice& device, const QImage& image, const TiffLayout& layout, QString& error) {
  if ((image.width() != int(layout.width)) || (image.height() != int(layout.height))) {
    error = SourceFile::tr("the decoded image does not match the file's size");
    return false;
  }

  // Byte order and BigTIFF as the source had them. 'B': most significant bit
  // first, which is what QImage::Format_Mono holds.
  QByteArray mode("wB");
  mode += layout.bigEndian ? 'b' : 'l';
  if (layout.bigTiff) {
    mode += '8';
  }
  TiffHandle handle(device, mode.constData());
  TIFF* const tif = handle.get();
  if (!tif) {
    error = SourceFile::tr("the TIFF writer could not be started (%1)").arg(handle.error());
    return false;
  }

  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, layout.width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, layout.height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, layout.bitsPerSample);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, layout.samplesPerPixel);
  if (layout.hasSampleFormat) {
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, layout.sampleFormat);
  }
  // Interleaved whatever the source had: separate planes describe the same pixels.
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, layout.photometric);
  if (layout.hasSubfileType) {
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, layout.subfileType);
  }

  TIFFSetField(tif, TIFFTAG_COMPRESSION, layout.compression);
  switch (layout.compression) {
    case COMPRESSION_CCITTFAX3:
      if (layout.hasT4Options) {
        TIFFSetField(tif, TIFFTAG_T4OPTIONS, layout.t4Options);
      }
      break;
    case COMPRESSION_CCITTFAX4:
      if (layout.hasT6Options) {
        TIFFSetField(tif, TIFFTAG_T6OPTIONS, layout.t6Options);
      }
      break;
    case COMPRESSION_JPEG:
      // The file does not record the quality it was made at. A high one keeps
      // the loss of this one re-encoding below what anyone could see.
      TIFFSetField(tif, TIFFTAG_JPEGQUALITY, 95);
      if (layout.photometric == PHOTOMETRIC_YCBCR) {
        TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, layout.ycbcrSubsampling[0], layout.ycbcrSubsampling[1]);
        TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
      }
      break;
    default:
      if (layout.hasPredictor) {
        TIFFSetField(tif, TIFFTAG_PREDICTOR, layout.predictor);
      }
      break;
  }

  if (layout.photometric == PHOTOMETRIC_PALETTE) {
    TIFFSetField(tif, TIFFTAG_COLORMAP, layout.colormap[0].data(), layout.colormap[1].data(),
                 layout.colormap[2].data());
  }
  if (layout.hasOrientation) {
    // The RGBA reader has already turned the pixels to the top-left corner.
    TIFFSetField(tif, TIFFTAG_ORIENTATION, layout.decodedAsRgba() ? uint16_t(ORIENTATION_TOPLEFT) : layout.orientation);
  }
  // A tiled source becomes striped: the same pixels, laid out the way the rest
  // of this application writes TIFF.
  const uint32_t rowsPerStrip
      = (!layout.tiled && layout.rowsPerStrip) ? layout.rowsPerStrip : TIFFDefaultStripSize(tif, 0);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rowsPerStrip);

  if (layout.hasResolution) {
    TIFFSetField(tif, TIFFTAG_XRESOLUTION, double(layout.xResolution));
    TIFFSetField(tif, TIFFTAG_YRESOLUTION, double(layout.yResolution));
    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, layout.resolutionUnit);
  }
  if (layout.hasPageNumber) {
    TIFFSetField(tif, TIFFTAG_PAGENUMBER, layout.pageNumber[0], layout.pageNumber[1]);
  }
  if (layout.hasPosition) {
    TIFFSetField(tif, TIFFTAG_XPOSITION, double(layout.xPosition));
    TIFFSetField(tif, TIFFTAG_YPOSITION, double(layout.yPosition));
  }
  for (const auto& tag : layout.asciiTags) {
    TIFFSetField(tif, tag.first, tag.second.constData());
  }
  for (const auto& tag : layout.blobTags) {
    TIFFSetField(tif, tag.first, uint32_t(tag.second.size() / blobUnit(tif, tag.first)), tag.second.constData());
  }

  // TIFFWriteScanline() may modify the buffer it is given, so lines go through a copy.
  const int width = image.width();
  const int height = image.height();
  std::vector<uint8_t> line;
  if (layout.samplesPerPixel == 1) {
    const bool bitonal = (layout.bitsPerSample == 1);
    const QImage::Format expected = bitonal ? QImage::Format_Mono : QImage::Format_Indexed8;
    if (image.format() != expected) {
      error = SourceFile::tr("the image was decoded to an unexpected format");
      return false;
    }
    // TiffReader keeps the file's sample values as the pixel indices, with a
    // palette that shows them correctly, so the indices are written back as they are.
    line.resize(bitonal ? size_t((width + 7) / 8) : size_t(width));
    for (int y = 0; y < height; ++y) {
      std::memcpy(line.data(), image.constScanLine(y), line.size());
      if (TIFFWriteScanline(tif, line.data(), uint32_t(y), 0) < 0) {
        error = SourceFile::tr("writing line %1 failed (%2)").arg(y).arg(handle.error());
        return false;
      }
    }
  } else {
    if ((image.format() != QImage::Format_RGB32) && (image.format() != QImage::Format_ARGB32)) {
      error = SourceFile::tr("the image was decoded to an unexpected format");
      return false;
    }
    line.resize(size_t(width) * 3);
    for (int y = 0; y < height; ++y) {
      const auto* src = reinterpret_cast<const QRgb*>(image.constScanLine(y));
      uint8_t* dst = line.data();
      for (int x = 0; x < width; ++x, dst += 3) {
        dst[0] = static_cast<uint8_t>(qRed(src[x]));
        dst[1] = static_cast<uint8_t>(qGreen(src[x]));
        dst[2] = static_cast<uint8_t>(qBlue(src[x]));
      }
      if (TIFFWriteScanline(tif, line.data(), uint32_t(y), 0) < 0) {
        error = SourceFile::tr("writing line %1 failed (%2)").arg(y).arg(handle.error());
        return false;
      }
    }
  }

  if (!TIFFWriteDirectory(tif)) {
    error = SourceFile::tr("the TIFF directory could not be written (%1)").arg(handle.error());
    return false;
  }
  handle.close();
  if (!handle.error().isEmpty()) {
    error = SourceFile::tr("libtiff: %1").arg(handle.error());
    return false;
  }
  return true;
}

/*================================ Common ================================*/

/**
 * Puts \p contents in the file, written in place. The new file is encoded in
 * full before the old one is touched, so a failure to encode changes nothing.
 */
bool replaceFile(const QString& path, const QByteArray& contents, QString& error) {
  if (!writeInPlace(path, contents, &error)) {
    error = SourceFile::tr(
                "%1. The copy of the original kept before the first retouch can be put back with "
                "Restore original.")
                .arg(error);
    return false;
  }
  return true;
}

QRect editsArea(const std::vector<Edit>& edits) {
  QRect area;
  for (const Edit& edit : edits) {
    area = area.united(edit.boundingRect());
  }
  return area;
}

bool rewriteTiff(const QString& path, const std::vector<Edit>& edits, QImage& edited, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = SourceFile::tr("could not open \"%1\" (%2)").arg(path, file.errorString());
    return false;
  }
  TiffLayout layout;
  if (!readTiffLayout(file, layout, error)) {
    return false;
  }
  error = tiffUnsupportedReason(layout);
  if (!error.isEmpty()) {
    return false;
  }
  file.seek(0);
  QImage image(TiffReader::readImage(file, 0));
  file.close();
  if (image.isNull()) {
    error = SourceFile::tr("the image could not be decoded");
    return false;
  }

  applyEdits(image, edits);

  QBuffer encoded;
  encoded.open(QIODevice::ReadWrite);
  if (!writeTiff(encoded, image, layout, error)) {
    return false;
  }
  if (!replaceFile(path, encoded.data(), error)) {
    return false;
  }
  edited = image;
  return true;
}

QImage decodeWithQt(const QByteArray& data, QByteArray* format = nullptr) {
  QBuffer buffer;
  buffer.setData(data);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  // The rest of the application reads files the same way: the pixels as
  // stored, not turned by an EXIF orientation.
  reader.setAutoTransform(false);
  if (format) {
    *format = reader.format();
  }
  return reader.read();
}

bool rewriteJpeg(const QString& path, const std::vector<Edit>& edits, QImage& edited, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = SourceFile::tr("could not open \"%1\" (%2)").arg(path, file.errorString());
    return false;
  }
  const QByteArray source(file.readAll());
  file.close();

  const QImage original(decodeWithQt(source));
  if (original.isNull()) {
    error = SourceFile::tr("the image could not be decoded");
    return false;
  }
  QImage image(original);
  applyEdits(image, edits);

  QByteArray patched;
  if (!patchJpeg(source, original, image, editsArea(edits), &patched, &error)) {
    return false;
  }
  if (!replaceFile(path, patched, error)) {
    return false;
  }
  // The decoded form of what was written, which differs a little from the
  // painted pixels: the changed blocks went through the JPEG quantizer.
  const QImage written(decodeWithQt(patched));
  edited = written.isNull() ? image : written;
  return true;
}

bool rewriteWithQt(const QString& path, const std::vector<Edit>& edits, QImage& edited, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = SourceFile::tr("could not open \"%1\" (%2)").arg(path, file.errorString());
    return false;
  }
  QByteArray format;
  QImage image(decodeWithQt(file.readAll(), &format));
  file.close();
  if (image.isNull()) {
    error = SourceFile::tr("the image could not be decoded");
    return false;
  }

  applyEdits(image, edits);

  QBuffer encoded;
  encoded.open(QIODevice::ReadWrite);
  // Text chunks, resolution and the colour profile travel with the QImage.
  QImageWriter writer(&encoded, format);
  if (!writer.write(image)) {
    error = SourceFile::tr("could not encode the image (%1)").arg(writer.errorString());
    return false;
  }
  if (!replaceFile(path, encoded.data(), error)) {
    return false;
  }
  edited = image;
  return true;
}
}  // namespace

QString SourceFile::unsupportedReason(const ImageId& imageId) {
  const QString path(imageId.filePath());
  const QFileInfo info(path);
  if (!info.exists()) {
    return tr("the file does not exist");
  }
  if (!info.isWritable()) {
    return tr("the file is read-only");
  }
  if (imageId.isMultiPageFile()) {
    return tr("it is one of several images in a multi-page file");
  }

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return tr("the file could not be opened (%1)").arg(file.errorString());
  }
  switch (detectFormat(file)) {
    case FileFormat::TIFF: {
      TiffLayout layout;
      QString error;
      if (!readTiffLayout(file, layout, error)) {
        return error;
      }
      return tiffUnsupportedReason(layout);
    }
    case FileFormat::JPEG:
      // The header is all that is needed, and it sits at the start of the file.
      return jpegUnsupportedReason(file.read(1 << 20));
    case FileFormat::PNG:
    case FileFormat::BMP:
      return QString();
    case FileFormat::UNKNOWN:
      break;
  }
  return tr("its file format is not one that can be retouched (TIFF, JPEG, PNG or BMP)");
}

bool SourceFile::rewrite(const ImageId& imageId, const std::vector<Edit>& edits, QImage* edited, QString* error) {
  DIAG_SCOPE(diagScope, "retouch.rewrite");
  const QString path(imageId.filePath());
  QString reason(unsupportedReason(imageId));
  QImage result;
  bool ok = false;
  if (reason.isEmpty()) {
    FileFormat format = FileFormat::UNKNOWN;
    {
      QFile file(path);
      if (file.open(QIODevice::ReadOnly)) {
        format = detectFormat(file);
      }
    }
    switch (format) {
      case FileFormat::TIFF:
        diagScope.attr(core::diag::Attr("format", "tiff"));
        ok = rewriteTiff(path, edits, result, reason);
        break;
      case FileFormat::JPEG:
        diagScope.attr(core::diag::Attr("format", "jpeg"));
        ok = rewriteJpeg(path, edits, result, reason);
        break;
      case FileFormat::PNG:
      case FileFormat::BMP:
        diagScope.attr(core::diag::Attr("format", format == FileFormat::PNG ? "png" : "bmp"));
        ok = rewriteWithQt(path, edits, result, reason);
        break;
      case FileFormat::UNKNOWN:
        reason = tr("the file could not be opened");
        break;
    }
  }
  diagScope.attr(core::diag::Attr("ok", ok));
  diagScope.attr(core::diag::Attr("edits", static_cast<int>(edits.size())));
  if (!ok) {
    if (error) {
      *error = reason;
    }
    return false;
  }
  if (edited) {
    *edited = result;
  }
  return true;
}
}  // namespace retouch
