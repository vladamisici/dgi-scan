// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_PROJECTHISTORY_H_
#define SCANTAILOR_CORE_PROJECTHISTORY_H_

#include <QDateTime>
#include <QString>
#include <vector>

namespace core {
/**
 * \brief The list of projects the operator has worked on, most recent first.
 *
 * This replaces a plain list of paths. Beyond remembering more of them, it keeps
 * enough about each - when it was last opened, how many pages it has, where its
 * output goes - for the start page to be worth reading rather than just a column
 * of file names.
 *
 * Entries whose file cannot currently be reached are deliberately KEPT. The
 * previous behaviour deleted them on startup, which meant that opening the
 * application while a network share happened to be unavailable silently threw
 * away the entire history. They are marked unavailable instead, and only removed
 * when the operator asks.
 */
class ProjectHistory {
 public:
  struct Entry {
    QString filePath;
    QString outputDirectory;
    QDateTime lastOpened;
    int pageCount = 0;

    bool isValid() const { return !filePath.isEmpty(); }

    /** \brief The project name, as shown to the operator. */
    QString displayName() const;

    /** \brief Whether the project file can be reached right now. */
    bool isAvailable() const;

    /** \brief "just now", "20 minutes ago", "yesterday", or a date. */
    QString lastOpenedDescription() const;
  };

  /** \brief How many entries are kept. */
  enum { MAX_ITEMS = 16 };

  /** \brief Loads the history, migrating the older path-only list if present. */
  void read();

  void write() const;

  /**
   * \brief Records that a project was opened or saved, moving it to the top.
   *
   * \p pageCount and \p outputDirectory may be left empty, in which case
   * anything already known about the project is kept.
   */
  void touch(const QString& filePath, int pageCount = 0, const QString& outputDirectory = QString());

  void remove(const QString& filePath);

  void clear();

  bool isEmpty() const { return m_entries.empty(); }

  const std::vector<Entry>& entries() const { return m_entries; }

  /** \brief Path of the most recently opened project, or an empty string. */
  QString mostRecent() const;

 private:
  std::vector<Entry> m_entries;
};
}  // namespace core

#endif  // ifndef SCANTAILOR_CORE_PROJECTHISTORY_H_
