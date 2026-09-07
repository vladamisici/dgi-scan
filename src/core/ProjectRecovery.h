// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_PROJECTRECOVERY_H_
#define SCANTAILOR_CORE_PROJECTRECOVERY_H_

#include <QDateTime>
#include <QString>

namespace core {
/**
 * \brief Locates and manages the recovery snapshot kept beside a project file.
 *
 * A snapshot is a complete project file written unattended while the operator
 * works. It is deliberately a sidecar rather than an overwrite of the project
 * itself: an unattended write must never be able to damage the last version the
 * operator deliberately saved.
 *
 * The presence of a snapshot after the project has been closed cleanly is
 * meaningful in itself - it means the previous session ended without ever
 * getting to a clean shutdown, i.e. the application crashed, the machine was
 * switched off, or a remote-desktop session was dropped. That is what makes it
 * possible to offer the operator their work back instead of silently losing it.
 *
 * The snapshot lives next to the project file, following the precedent already
 * set by the "Backup.<name>" file the application writes when closing a project,
 * so that it can also be recovered by hand: renaming the snapshot over the
 * project file is all it takes.
 */
class ProjectRecovery {
 public:
  /**
   * \brief Path of the snapshot belonging to \p projectFilePath.
   *
   * Returns an empty string for an empty input (an unnamed project has nowhere
   * to put a snapshot).
   */
  static QString snapshotPathFor(const QString& projectFilePath);

  /** \brief Whether a snapshot exists and is non-empty. */
  static bool hasSnapshot(const QString& projectFilePath);

  /** \brief Modification time of the snapshot, or an invalid QDateTime. */
  static QDateTime snapshotTimestamp(const QString& projectFilePath);

  /**
   * \brief Whether the snapshot holds work the project file does not.
   *
   * True when a snapshot exists and is strictly newer than the project file, or
   * when the project file does not exist at all. A snapshot that is older than
   * the project has been superseded by a deliberate save and is uninteresting.
   */
  static bool isSnapshotNewerThanProject(const QString& projectFilePath);

  /** \brief Removes the snapshot. Succeeds if there was nothing to remove. */
  static bool discardSnapshot(const QString& projectFilePath);

  /**
   * \brief Path of the snapshot used for a project that has never been saved.
   *
   * A project created from a folder of scans has no file to sit beside until the
   * operator does Save As, so its snapshot goes to a fixed per-user location
   * instead. Without it, an hour of work on a new project - Fix DPI, page
   * splitting, content corrections - is protected by nothing at all.
   *
   * The file is an ordinary project file and can simply be opened.
   */
  static QString unsavedSessionPath();

  /** \brief Whether an unsaved session from a previous run is waiting. */
  static bool hasUnsavedSession();

  /** \brief Removes the unsaved-session snapshot. */
  static bool discardUnsavedSession();

  ProjectRecovery() = delete;
};
}  // namespace core

#endif  // ifndef SCANTAILOR_CORE_PROJECTRECOVERY_H_
