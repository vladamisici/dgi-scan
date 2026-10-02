// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_RETOUCH_JPEGPATCH_H_
#define SCANTAILOR_RETOUCH_JPEGPATCH_H_

#include <QByteArray>
#include <QRect>
#include <QString>

class QImage;

namespace retouch {
/**
 * \brief Why the JPEG file held in \p data cannot be patched, or an empty string if it can.
 *
 * Only the header is read.
 */
QString jpegUnsupportedReason(const QByteArray& data);

/**
 * \brief Rewrites the JPEG \p source so that it decodes to \p edited.
 *
 * Works on the compressed data, the way lossless JPEG tools do, rather than
 * decoding and re-encoding the image: only the 8x8 blocks in which \p edited
 * differs from \p original are encoded again - with the file's own quantization
 * tables and chroma subsampling - and every other block keeps its coefficients
 * exactly. The markers - JFIF density, EXIF, the ICC profile, comments - are
 * carried over as they are.
 *
 * \param original \p source decoded, as the rest of the application decodes it.
 * \param edited \p original with the edits painted in: same size and format.
 * \param changedArea Where \p edited may differ from \p original. Pixels outside
 *        it are not compared.
 * \param result Receives the new file. Set to \p source if nothing changed.
 * \param error Receives the reason on failure. May be null.
 */
bool patchJpeg(const QByteArray& source,
               const QImage& original,
               const QImage& edited,
               const QRect& changedArea,
               QByteArray* result,
               QString* error);
}  // namespace retouch

#endif  // SCANTAILOR_RETOUCH_JPEGPATCH_H_
