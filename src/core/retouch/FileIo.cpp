// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "FileIo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#include <cerrno>
#endif

namespace retouch {
namespace {
struct Tr {
  Q_DECLARE_TR_FUNCTIONS(retouch::FileIo)
};
}  // namespace

bool openForOverwrite(QFile& file, QString* error) {
  // Some two seconds in all: long enough for a scanner to let go of a file,
  // short enough not to be mistaken for a hang.
  static const int delaysMs[] = {0, 50, 150, 300, 600, 1000};
  for (const int delay : delaysMs) {
    if (delay > 0) {
      QThread::msleep(static_cast<unsigned long>(delay));
    }
    if (file.open(QIODevice::WriteOnly)) {
      return true;
    }
  }
  if (error) {
    *error = Tr::tr("could not open \"%1\" for writing (%2)")
                 .arg(QDir::toNativeSeparators(file.fileName()), file.errorString());
  }
  return false;
}

bool syncToDisk(QFile& file) {
  if (!file.flush()) {
    return false;
  }
  const int fd = file.handle();
  if (fd < 0) {
    return true;
  }
#ifdef Q_OS_WIN
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if ((handle == INVALID_HANDLE_VALUE) || ::FlushFileBuffers(handle)) {
    return true;
  }
  const DWORD error = ::GetLastError();
  return (error == ERROR_INVALID_FUNCTION) || (error == ERROR_NOT_SUPPORTED);
#else
  if (::fsync(fd) == 0) {
    return true;
  }
  return (errno == EINVAL) || (errno == ENOTSUP) || (errno == EBADF);
#endif
}

bool writeInPlace(const QString& path, const QByteArray& bytes, QString* error) {
  QFile file(path);
  if (!openForOverwrite(file, error)) {
    return false;
  }
  const auto fail = [&](const QString& reason) {
    if (error) {
      *error = reason;
    }
    return false;
  };
  if (file.write(bytes) != bytes.size()) {
    return fail(Tr::tr("writing \"%1\" failed (%2)").arg(QDir::toNativeSeparators(path), file.errorString()));
  }
  if (!syncToDisk(file) || (file.error() != QFileDevice::NoError)) {
    return fail(
        Tr::tr("\"%1\" could not be flushed to the disk (%2)").arg(QDir::toNativeSeparators(path), file.errorString()));
  }
  file.close();
  // A write cut short by a dropped connection can still have reported success.
  if (QFileInfo(path).size() != bytes.size()) {
    return fail(Tr::tr("\"%1\" came out %2 bytes long instead of %3")
                    .arg(QDir::toNativeSeparators(path))
                    .arg(QFileInfo(path).size())
                    .arg(bytes.size()));
  }
  return true;
}
}  // namespace retouch
