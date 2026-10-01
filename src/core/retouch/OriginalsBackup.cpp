// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "OriginalsBackup.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <vector>

#include "Diagnostics.h"
#include "FileIo.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace retouch {
namespace {
/** The same file however it was spelled, for hashing. */
QString canonicalSpelling(const QString& path) {
  QString spelling(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
#ifdef Q_OS_WIN
  // Windows paths are not case-sensitive, and neither are shares on Windows servers.
  spelling = spelling.toCaseFolded();
#endif
  return spelling;
}

/**
 * Copies \p from to \p to, which must not exist yet, the fast way: on Windows
 * the copy is done by the file server itself when both are on the same share,
 * instead of every byte crossing the network twice.
 */
bool copyNewFile(const QString& from, const QString& to) {
#ifdef Q_OS_WIN
  return ::CopyFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(from).utf16()),
                     reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(to).utf16()), TRUE)
         != 0;
#else
  (void) from;
  (void) to;
  return false;
#endif
}
}  // namespace

OriginalsBackup::OriginalsBackup(const QString& outputDirectory)
    : m_directory(QDir(outputDirectory).absoluteFilePath(QStringLiteral("cache/retouch-originals"))) {}

QString OriginalsBackup::pathFor(const QString& sourcePath) const {
  const QFileInfo info(sourcePath);
  const QByteArray hash(
      QCryptographicHash::hash(canonicalSpelling(sourcePath).toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
  QString name(info.completeBaseName() + QLatin1Char('.') + QString::fromLatin1(hash));
  if (!info.suffix().isEmpty()) {
    name += QLatin1Char('.') + info.suffix();
  }
  return QDir(m_directory).absoluteFilePath(name);
}

QString OriginalsBackup::markerFor(const QString& copyPath) {
  return copyPath + QStringLiteral(".complete");
}

bool OriginalsBackup::contains(const QString& sourcePath) const {
  const QString copyPath(pathFor(sourcePath));
  return QFileInfo::exists(markerFor(copyPath)) && QFileInfo::exists(copyPath);
}

bool OriginalsBackup::copyInPlace(const QString& from, const QString& to, QString* error) {
  const auto fail = [error](const QString& reason) {
    if (error) {
      *error = reason;
    }
    return false;
  };
  QFile source(from);
  if (!source.open(QIODevice::ReadOnly)) {
    return fail(tr("could not open \"%1\" (%2)").arg(QDir::toNativeSeparators(from), source.errorString()));
  }
  QFile target(to);
  if (!openForOverwrite(target, error)) {
    return false;
  }
  std::vector<char> buffer(size_t(4) << 20);
  while (true) {
    const qint64 read = source.read(buffer.data(), qint64(buffer.size()));
    if (read < 0) {
      return fail(tr("could not read \"%1\" (%2)").arg(QDir::toNativeSeparators(from), source.errorString()));
    }
    if (read == 0) {
      break;
    }
    if (target.write(buffer.data(), read) != read) {
      return fail(tr("could not write \"%1\" (%2)").arg(QDir::toNativeSeparators(to), target.errorString()));
    }
  }
  if (!syncToDisk(target) || (target.error() != QFileDevice::NoError)) {
    return fail(
        tr("\"%1\" could not be flushed to the disk (%2)").arg(QDir::toNativeSeparators(to), target.errorString()));
  }
  const qint64 expected = source.size();
  target.close();
  source.close();
  if (QFileInfo(to).size() != expected) {
    return fail(tr("the copy of \"%1\" came out incomplete").arg(QDir::toNativeSeparators(from)));
  }
  return true;
}

bool OriginalsBackup::keep(const QString& sourcePath, QString* error) {
  DIAG_SCOPE(diagScope, "retouch.backup.keep");
  const QString copyPath(pathFor(sourcePath));
  const QString marker(markerFor(copyPath));
  if (QFileInfo::exists(marker) && QFileInfo::exists(copyPath)) {
    diagScope.attr(core::diag::Attr("copied", false));
    return true;
  }
  if (!QDir().mkpath(m_directory)) {
    if (error) {
      *error = tr("could not create the folder \"%1\"").arg(QDir::toNativeSeparators(m_directory));
    }
    return false;
  }

  // A copy left without its marker was cut short, and gets written over in
  // place. Only a fresh one can take the fast route, which will not overwrite.
  bool copied = !QFileInfo::exists(copyPath) && copyNewFile(sourcePath, copyPath)
                && (QFileInfo(copyPath).size() == QFileInfo(sourcePath).size());
  if (!copied && !copyInPlace(sourcePath, copyPath, error)) {
    return false;
  }

  QFile completed(marker);
  if (!completed.open(QIODevice::WriteOnly)) {
    if (error) {
      *error = tr("could not create \"%1\" (%2)").arg(QDir::toNativeSeparators(marker), completed.errorString());
    }
    return false;
  }
  completed.close();
  diagScope.attr(core::diag::Attr("copied", true));
  log(QStringLiteral("kept"), sourcePath, copyPath);
  return true;
}

bool OriginalsBackup::restore(const QString& sourcePath, QString* error) {
  DIAG_SCOPE(diagScope, "retouch.backup.restore");
  if (!contains(sourcePath)) {
    if (error) {
      *error = tr("no copy of the original is kept for \"%1\"").arg(QDir::toNativeSeparators(sourcePath));
    }
    return false;
  }
  const QString copyPath(pathFor(sourcePath));
  // In place: a copy and a rename would need the right to rename, which the
  // shares this runs on can refuse. If this is cut short the copy is still
  // here, and restoring again finishes the job.
  if (!copyInPlace(copyPath, sourcePath, error)) {
    return false;
  }
  log(QStringLiteral("restored"), sourcePath, copyPath);
  return true;
}

void OriginalsBackup::log(const QString& action, const QString& sourcePath, const QString& copyPath) const {
  // Best effort: the index helps someone looking through the folder by hand,
  // and nothing reads it back.
  QFile index(QDir(m_directory).absoluteFilePath(QStringLiteral("index.txt")));
  if (!index.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    return;
  }
  QTextStream out(&index);
  out.setCodec("UTF-8");
  out << QDateTime::currentDateTime().toString(Qt::ISODate) << '\t' << action << '\t'
      << QDir::toNativeSeparators(sourcePath) << '\t' << QFileInfo(copyPath).fileName() << '\n';
}
}  // namespace retouch
