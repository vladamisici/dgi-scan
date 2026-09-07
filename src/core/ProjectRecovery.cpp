// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "ProjectRecovery.h"

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

bool ProjectRecovery::discardSnapshot(const QString& projectFilePath) {
  const QString path = snapshotPathFor(projectFilePath);
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    return true;
  }
  return QFile::remove(path);
}
}  // namespace core
