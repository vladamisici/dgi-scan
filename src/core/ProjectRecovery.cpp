// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "ProjectRecovery.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace core {
namespace {
const QLatin1String SNAPSHOT_SUFFIX(".autosave");
}

QString ProjectRecovery::snapshotPathFor(const QString& projectFilePath) {
  if (projectFilePath.isEmpty()) {
    return QString();
  }
  return projectFilePath + SNAPSHOT_SUFFIX;
}

bool ProjectRecovery::hasSnapshot(const QString& projectFilePath) {
  const QString path = snapshotPathFor(projectFilePath);
  if (path.isEmpty()) {
    return false;
  }
  const QFileInfo info(path);
  // A zero-length snapshot is the residue of a write that was itself
  // interrupted; treating it as recoverable work would only offer the operator
  // an empty project.
  return info.exists() && info.isFile() && (info.size() > 0);
}

QDateTime ProjectRecovery::snapshotTimestamp(const QString& projectFilePath) {
  const QString path = snapshotPathFor(projectFilePath);
  if (path.isEmpty()) {
    return QDateTime();
  }
  return QFileInfo(path).lastModified();
}

bool ProjectRecovery::isSnapshotNewerThanProject(const QString& projectFilePath) {
  if (!hasSnapshot(projectFilePath)) {
    return false;
  }

  const QFileInfo projectInfo(projectFilePath);
  if (!projectInfo.exists()) {
    return true;
  }
  return snapshotTimestamp(projectFilePath) > projectInfo.lastModified();
}

QString ProjectRecovery::localSnapshotPathFor(const QString& projectFilePath) {
  if (projectFilePath.isEmpty()) {
    return QString();
  }
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (dir.isEmpty()) {
    return QString();
  }
  // The full path is hashed into the name so that two projects with the same
  // file name do not overwrite one another, while the name stays recognisable.
  const QString digest = QString::fromLatin1(
      QCryptographicHash::hash(projectFilePath.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
  return dir + QLatin1String("/recovery/") + QFileInfo(projectFilePath).completeBaseName() + QLatin1Char('-') + digest
         + QLatin1String(".ScanTailor");
}

QString ProjectRecovery::unsavedSessionPath() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (dir.isEmpty()) {
    return QString();
  }
  return dir + QLatin1String("/recovery/unsaved-session.ScanTailor");
}

bool ProjectRecovery::hasUnsavedSession() {
  const QString path = unsavedSessionPath();
  if (path.isEmpty()) {
    return false;
  }
  const QFileInfo info(path);
  return info.exists() && info.isFile() && (info.size() > 0);
}

bool ProjectRecovery::discardUnsavedSession() {
  const QString path = unsavedSessionPath();
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    return true;
  }
  return QFile::remove(path);
}

QString ProjectRecovery::preserveUnsavedSession() {
  if (!hasUnsavedSession()) {
    return QString();
  }

  const QString path = unsavedSessionPath();
  const QFileInfo info(path);
  const QString stem = info.absolutePath() + QLatin1String("/") + info.completeBaseName()
                       + QLatin1String("-kept-") + info.lastModified().toString(QLatin1String("yyyyMMdd-hhmmss"));

  // Numbered rather than overwritten, so several orphans can coexist.
  for (int i = 0; i < 100; ++i) {
    const QString candidate
        = (i == 0) ? (stem + QLatin1String(".ScanTailor"))
                   : (stem + QLatin1String("-") + QString::number(i) + QLatin1String(".ScanTailor"));
    if (!QFileInfo::exists(candidate) && QFile::rename(path, candidate)) {
      return candidate;
    }
  }
  return QString();
}

bool ProjectRecovery::discardSnapshot(const QString& projectFilePath) {
  const QString path = snapshotPathFor(projectFilePath);
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    return true;
  }
  return QFile::remove(path);
}
}  // namespace core
