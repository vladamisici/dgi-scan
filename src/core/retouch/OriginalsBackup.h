// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_RETOUCH_ORIGINALSBACKUP_H_
#define SCANTAILOR_RETOUCH_ORIGINALSBACKUP_H_

#include <QCoreApplication>
#include <QString>

namespace retouch {
/**
 * \brief Keeps a copy of each image file as it was before it was first retouched.
 *
 * Retouching writes over scans, and a slip of the brush that is saved is
 * otherwise permanent. The copies live in the project's output folder, under
 * cache/retouch-originals, which is where the rest of the project's working
 * files already are and which nothing downstream takes pages from. Each copy
 * is named after its file plus a hash of the file's full path, so that two
 * scans with the same name from different folders do not collide, and an
 * index.txt beside them records where each one came from.
 *
 * Only the first retouch of a file is copied: a later one must not replace the
 * untouched original with an already retouched version.
 *
 * Nothing here renames or deletes a file. The shares this runs on can allow
 * creating and writing files while refusing exactly those two - which is what
 * left an earlier version unable to keep a copy at all. A copy is therefore
 * written straight to its final name, and only counts as kept once a marker
 * beside it says it was completed: a copy cut short is simply written over by
 * the next attempt. Restoring writes the original back over the file in place,
 * and the copy stays where it is.
 */
class OriginalsBackup {
  Q_DECLARE_TR_FUNCTIONS(retouch::OriginalsBackup)

 public:
  explicit OriginalsBackup(const QString& outputDirectory);

  const QString& directory() const { return m_directory; }

  /** \brief Where the copy of \p sourcePath is, or would be, kept. */
  QString pathFor(const QString& sourcePath) const;

  /** \brief Whether a complete copy of \p sourcePath is kept. */
  bool contains(const QString& sourcePath) const;

  /** \brief Copies \p sourcePath here, unless a complete copy is kept already. */
  bool keep(const QString& sourcePath, QString* error);

  /**
   * \brief Writes the kept copy back over \p sourcePath.
   *
   * The copy is kept: it is the original, and the file can be retouched again.
   */
  bool restore(const QString& sourcePath, QString* error);

  /**
   * \brief Copies \p from over \p to, writing \p to in place.
   *
   * \p to is created if it does not exist and truncated if it does - never
   * deleted or renamed. Flushed to the disk, and checked for size, before
   * reporting success.
   */
  static bool copyInPlace(const QString& from, const QString& to, QString* error);

 private:
  static QString markerFor(const QString& copyPath);

  void log(const QString& action, const QString& sourcePath, const QString& copyPath) const;

  QString m_directory;
};
}  // namespace retouch

#endif  // SCANTAILOR_RETOUCH_ORIGINALSBACKUP_H_
