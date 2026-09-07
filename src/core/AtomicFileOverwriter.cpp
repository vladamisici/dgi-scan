// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "AtomicFileOverwriter.h"

#include <QDebug>
#include <QFile>
#include <QTemporaryFile>

#include "Utils.h"

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace core;

namespace {
/**
 * \brief Asks the OS to put the file's contents on the storage device.
 *
 * A durability barrier, not a write: it makes the rename in commit() safe
 * against the machine losing power or a network share dropping between the data
 * being handed to the OS and the rename landing.
 *
 * Deliberately advisory. Some filesystems and network redirectors do not
 * implement the barrier and fail the call even though the write itself is fine,
 * and refusing to save on those would be a worse bug than the one this guards
 * against. Genuine write failures are caught separately, by flushing the file
 * and inspecting its error state.
 */
bool syncToDisk(QFile& file) {
  const int fd = file.handle();
  if (fd < 0) {
    return false;
  }

#ifdef Q_OS_WIN
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  return ::FlushFileBuffers(handle) != 0;
#else
  return ::fsync(fd) == 0;
#endif
}
}  // namespace

AtomicFileOverwriter::AtomicFileOverwriter() = default;

AtomicFileOverwriter::~AtomicFileOverwriter() {
  abort();
}

QIODevice* AtomicFileOverwriter::startWriting(const QString& filePath) {
  abort();

  m_tempFile = std::make_unique<QTemporaryFile>(filePath);
  m_tempFile->setAutoRemove(false);
  if (!m_tempFile->open()) {
    m_tempFile.reset();
  }
  return m_tempFile.get();
}

bool AtomicFileOverwriter::commit() {
  if (!m_tempFile) {
    return false;
  }

  const QString tempFilePath(m_tempFile->fileName());
  const QString targetPath(m_tempFile->fileTemplate());

  // A write error that only surfaces at flush time - a full disk, a quota, a
  // share that went away mid-write - must not be promoted into a rename over
  // good data.
  const bool written = m_tempFile->flush() && (m_tempFile->error() == QFileDevice::NoError);
  if (written && !syncToDisk(*m_tempFile)) {
    // Advisory only: see syncToDisk(). The data is written; it may just not be
    // guaranteed on the platter yet.
    qWarning() << "Could not flush" << tempFilePath << "to disk; continuing without a durability barrier";
  }

  // Yes, we have to destroy this object here, because:
  // 1. Under Windows, open files can't be renamed or deleted.
  // 2. QTemporaryFile::close() doesn't really close it.
  m_tempFile.reset();

  if (!written) {
    QFile::remove(tempFilePath);
    return false;
  }

  if (!Utils::overwritingRename(tempFilePath, targetPath)) {
    QFile::remove(tempFilePath);
    return false;
  }
  return true;
}

void AtomicFileOverwriter::abort() {
  if (!m_tempFile) {
    return;
  }

  const QString tempFilePath(m_tempFile->fileName());
  m_tempFile.reset();  // See comments in commit()
  QFile::remove(tempFilePath);
}
