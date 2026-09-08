// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "ProjectHistory.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <algorithm>

namespace core {
namespace {
const QLatin1String HISTORY_KEY("project/history");
// The path-only list written by earlier versions, read once so that upgrading
// does not look like the history was lost.
const QLatin1String LEGACY_KEY("project/recent");
}  // namespace

QString ProjectHistory::Entry::displayName() const {
  const QString name = QFileInfo(filePath).completeBaseName();
  return name.isEmpty() ? filePath : name;
}

bool ProjectHistory::Entry::isAvailable() const {
  return !filePath.isEmpty() && QFileInfo::exists(filePath);
}

QString ProjectHistory::Entry::lastOpenedDescription() const {
  if (!lastOpened.isValid()) {
    return QCoreApplication::translate("ProjectHistory", "never opened");
  }

  const QDateTime now = QDateTime::currentDateTime();
  const qint64 seconds = lastOpened.secsTo(now);

  if (seconds < 0) {
    // A clock change, or a project file copied from another machine.
    return QLocale().toString(lastOpened, QLocale::ShortFormat);
  }
  if (seconds < 60) {
    return QCoreApplication::translate("ProjectHistory", "just now");
  }
  // Written out rather than using tr()'s %n plural form: with no translator
  // loaded Qt keeps the source string verbatim, which reads as "1 minute(s)".
  if (seconds < 60 * 60) {
    const int minutes = static_cast<int>(seconds / 60);
    return (minutes == 1) ? QCoreApplication::translate("ProjectHistory", "a minute ago")
                          : QCoreApplication::translate("ProjectHistory", "%1 minutes ago").arg(minutes);
  }
  if (seconds < 24 * 60 * 60) {
    const int hours = static_cast<int>(seconds / (60 * 60));
    return (hours == 1) ? QCoreApplication::translate("ProjectHistory", "an hour ago")
                        : QCoreApplication::translate("ProjectHistory", "%1 hours ago").arg(hours);
  }
  if (lastOpened.date().daysTo(now.date()) == 1) {
    return QCoreApplication::translate("ProjectHistory", "yesterday");
  }
  if (seconds < 7 * 24 * 60 * 60) {
    const int days = static_cast<int>(seconds / (24 * 60 * 60));
    return QCoreApplication::translate("ProjectHistory", "%1 days ago").arg(days);
  }
  return QLocale().toString(lastOpened.date(), QLocale::ShortFormat);
}

void ProjectHistory::read() {
  QSettings settings;
  std::vector<Entry> loaded;

  const int size = settings.beginReadArray(HISTORY_KEY);
  for (int i = 0; i < size; ++i) {
    settings.setArrayIndex(i);
    Entry entry;
    entry.filePath = settings.value(QLatin1String("path")).toString();
    if (entry.filePath.isEmpty()) {
      continue;
    }
    entry.outputDirectory = settings.value(QLatin1String("outputDirectory")).toString();
    entry.lastOpened = settings.value(QLatin1String("lastOpened")).toDateTime();
    entry.pageCount = settings.value(QLatin1String("pageCount"), 0).toInt();
    loaded.push_back(entry);
  }
  settings.endArray();

  if (loaded.empty()) {
    const int legacySize = settings.beginReadArray(LEGACY_KEY);
    for (int i = 0; i < legacySize; ++i) {
      settings.setArrayIndex(i);
      Entry entry;
      entry.filePath = settings.value(QLatin1String("path")).toString();
      if (!entry.filePath.isEmpty()) {
        loaded.push_back(entry);
      }
    }
    settings.endArray();
  }

  m_entries.swap(loaded);
}

void ProjectHistory::write() const {
  QSettings settings;
  settings.beginWriteArray(HISTORY_KEY);
  int index = 0;
  for (const Entry& entry : m_entries) {
    if (index >= MAX_ITEMS) {
      break;
    }
    settings.setArrayIndex(index);
    settings.setValue(QLatin1String("path"), entry.filePath);
    settings.setValue(QLatin1String("outputDirectory"), entry.outputDirectory);
    settings.setValue(QLatin1String("lastOpened"), entry.lastOpened);
    settings.setValue(QLatin1String("pageCount"), entry.pageCount);
    ++index;
  }
  settings.endArray();
}

void ProjectHistory::touch(const QString& filePath, const int pageCount, const QString& outputDirectory) {
  if (filePath.isEmpty()) {
    return;
  }

  Entry entry;
  const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                               [&filePath](const Entry& e) { return e.filePath == filePath; });
  if (it != m_entries.end()) {
    entry = *it;
    m_entries.erase(it);
  }

  entry.filePath = filePath;
  entry.lastOpened = QDateTime::currentDateTime();
  // Only overwrite what the caller actually knows: saving a project knows the
  // page count, while relinking it may not.
  if (pageCount > 0) {
    entry.pageCount = pageCount;
  }
  if (!outputDirectory.isEmpty()) {
    entry.outputDirectory = outputDirectory;
  }

  m_entries.insert(m_entries.begin(), entry);
  if (static_cast<int>(m_entries.size()) > MAX_ITEMS) {
    m_entries.resize(MAX_ITEMS);
  }
}

void ProjectHistory::remove(const QString& filePath) {
  m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                 [&filePath](const Entry& e) { return e.filePath == filePath; }),
                  m_entries.end());
}

void ProjectHistory::clear() {
  m_entries.clear();
}

QString ProjectHistory::mostRecent() const {
  return m_entries.empty() ? QString() : m_entries.front().filePath;
}
}  // namespace core
