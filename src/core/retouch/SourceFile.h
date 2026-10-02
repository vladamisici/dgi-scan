// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_RETOUCH_SOURCEFILE_H_
#define SCANTAILOR_RETOUCH_SOURCEFILE_H_

#include <QCoreApplication>
#include <QString>
#include <vector>

#include "Edit.h"

class ImageId;
class QImage;

namespace retouch {
/**
 * \brief Writes edits into the image files a project is built from.
 *
 * A source file is a scan, and nothing but the painted-over pixels may change
 * when one is rewritten. The file keeps its format and everything that can be
 * carried over with the pixels: bit depth, colour space, compression and its
 * options, resolution, colour profile and descriptive tags.
 *
 *  - TIFF is re-encoded with the source file's own parameters. Lossless
 *    compressions lose nothing; a JPEG-compressed TIFF is re-encoded at high
 *    quality.
 *  - JPEG is patched rather than re-encoded: only the 8x8 blocks whose pixels
 *    actually changed are encoded again, with the file's own quantization
 *    tables. Every other block, and every marker - EXIF, ICC profile, comments -
 *    is carried over bit for bit.
 *  - PNG and BMP are lossless, and written through Qt.
 *
 * The new file is encoded in full in memory and then written over the old one
 * in place, not beside it and renamed: the shares this runs on can allow
 * writing a file while refusing to rename or delete one, and a temporary file
 * that cannot be removed would be left among the scans. A write cut short
 * leaves the file damaged, which is why callers keep a copy of the original
 * first - see OriginalsBackup.
 */
class SourceFile {
  Q_DECLARE_TR_FUNCTIONS(retouch::SourceFile)

 public:
  /**
   * \brief Why the file of \p imageId cannot take edits, or an empty string if it can.
   *
   * Turns down what could not be written back without losing more than the
   * painted-over pixels: images within multi-page files, 16-bit samples, CMYK,
   * transparency, read-only files. Reads only the file's header.
   */
  static QString unsupportedReason(const ImageId& imageId);

  /**
   * \brief Paints \p edits into the file of \p imageId, writing it over in place.
   *
   * \param edited Receives the image as written, decoded the way the rest of the
   *        application decodes the file. May be null.
   * \param error Receives the reason on failure. May be null.
   * \return Whether the file was written. A failure before writing leaves the
   *         file as it was; one during the write may not.
   */
  static bool rewrite(const ImageId& imageId, const std::vector<Edit>& edits, QImage* edited, QString* error);
};
}  // namespace retouch

#endif  // SCANTAILOR_RETOUCH_SOURCEFILE_H_
