// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "AtomicFileOverwriter.h"

#include <QDebug>
#include <QFile>
#include <QObject>
#include <QTemporaryFile>

#include "Utils.h"

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
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
enum class SyncResult {
  Synced,        /**< The data is on the storage device. */
  Unsupported,   /**< This filesystem has no flush barrier; the write is still fine. */
  Failed         /**< The flush genuinely failed - the data may not be there. */
};

SyncResult syncToDisk(QFile& file) {
  const int fd = file.handle();
  if (fd < 0) {
    return SyncResult::Unsupported;
  }

#ifdef Q_OS_WIN
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return SyncResult::Unsupported;
  }
  if (::FlushFileBuffers(handle) != 0) {
    return SyncResult::Synced;
  }
  const DWORD error = ::GetLastError();
  // Some filesystems and network redirectors simply do not implement the
  // barrier and say so. That is not a write failure and must not stop the save.
  if ((error == ERROR_INVALID_FUNCTION) || (error == ERROR_NOT_SUPPORTED)) {
    return SyncResult::Unsupported;
  }
  return SyncResult::Failed;
#else
  if (::fsync(fd) == 0) {
    return SyncResult::Synced;
  }
  if ((errno == EINVAL) || (errno == ENOTSUP) || (errno == EBADF)) {
    return SyncResult::Unsupported;
  }
  return SyncResult::Failed;
#endif
}
}  // namespace

AtomicFileOverwriter::AtomicFileOverwriter() = default;

AtomicFileOverwriter::~AtomicFileOverwriter() {
  abort();
}

QIODevice* AtomicFileOverwriter::startWriting(const QString& filePath) {
  abort();
  m_errorString.clear();
  m_failureStage = FailureStage::None;

  m_tempFile = std::make_unique<QTemporaryFile>(filePath);
  m_tempFile->setAutoRemove(false);
  if (!m_tempFile->open()) {
    // Worth spelling out, because this is the step that can fail where simply
    // overwriting the existing file would have worked: writing here needs
    // permission to create a NEW file in the directory, which a share can
    // withhold while still allowing an existing file to be modified.
    m_errorString = QObject::tr("could not create a temporary file next to \"%1\" (%2); "
                                "saving this way needs permission to create files in that folder")
                        .arg(filePath, m_tempFile->errorString());
    m_failureStage = FailureStage::Create;
    m_tempFile.reset();
  }
  return m_tempFile.get();
}

bool AtomicFileOverwriter::commit() {
  if (!m_tempFile) {
    m_errorString = QObject::tr("nothing was being written");
    m_failureStage = FailureStage::Write;
    return false;
  }
  m_errorString.clear();
  m_failureStage = FailureStage::None;

  const QString tempFilePath(m_tempFile->fileName());
  const QString targetPath(m_tempFile->fileTemplate());

  // A write error that only surfaces at flush time - a full disk, a quota, a
  // share that went away mid-write - must not be promoted into a rename over
  // good data.
  bool written = m_tempFile->flush() && (m_tempFile->error() == QFileDevice::NoError);
  if (!written) {
    m_errorString = QObject::tr("could not write \"%1\" (%2)").arg(tempFilePath, m_tempFile->errorString());
    m_failureStage = FailureStage::Write;
  }
  if (written) {
    switch (syncToDisk(*m_tempFile)) {
      case SyncResult::Synced:
        break;
      case SyncResult::Unsupported:
        // The data is written; it may just not be guaranteed on the platter yet.
        // Refusing to save here would break saving outright on such filesystems.
        break;
      case SyncResult::Failed:
        // A real flush failure means the bytes may never reach the share. Renaming
        // this file over the operator's project would be the exact data loss the
        // atomic write exists to prevent.
        m_errorString = QObject::tr("could not flush \"%1\" to disk (%2)")
                            .arg(tempFilePath, Utils::lastSystemErrorString());
        m_failureStage = FailureStage::Write;
        qCritical() << "Failed to flush" << tempFilePath << "to disk; the save is being abandoned";
        written = false;
        break;
    }
  }

  // Yes, we have to destroy this object here, because:
  // 1. Under Windows, open files can't be renamed or deleted.
  // 2. QTemporaryFile::close() doesn't really close it.
  m_tempFile.reset();

  if (!written) {
    QFile::remove(tempFilePath);
    return false;
  }

  QString renameError;
  if (!Utils::overwritingRename(tempFilePath, targetPath, &renameError)) {
    m_errorString
        = QObject::tr("could not replace \"%1\" with the file just written (%2)").arg(targetPath, renameError);
    m_failureStage = FailureStage::Replace;
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
