// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_RETOUCH_FILEIO_H_
#define SCANTAILOR_RETOUCH_FILEIO_H_

#include <QByteArray>
#include <QString>

class QFile;

namespace retouch {
/**
 * \brief Opens \p file for writing over what it holds, creating it if need be.
 *
 * Retries for a moment while something else - a virus scanner, the thumbnail
 * cache of Explorer - has the file open, which on a share happens often enough
 * to matter and passes quickly.
 */
bool openForOverwrite(QFile& file, QString* error);

/**
 * \brief Asks the operating system to put what was written to \p file on the disk.
 *
 * \return false only if the flush genuinely failed. A filesystem or network
 *         redirector that has no flush at all is not a failure.
 */
bool syncToDisk(QFile& file);

/**
 * \brief Replaces the contents of \p path with \p bytes, writing the file in place.
 *
 * No temporary file, no rename: the file keeps its identity, and the write
 * needs no more than permission to write it. The data is flushed to the disk,
 * and the file's size checked, before success is reported.
 */
bool writeInPlace(const QString& path, const QByteArray& bytes, QString* error);
}  // namespace retouch

#endif  // SCANTAILOR_RETOUCH_FILEIO_H_
