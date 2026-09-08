// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "Utils.h"

#include <QDir>
#include <QRegularExpression>
#include <QTextDocument>
#include <cmath>

#include "ApplicationSettings.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <stdio.h>
#endif

namespace core {
bool Utils::overwritingRename(const QString& from, const QString& to, QString* errorMessage) {
#ifdef Q_OS_WIN
  // Retried briefly. A virus scanner, the search indexer or a backup agent that
  // holds either file open for a moment without sharing delete access makes the
  // replace fail transiently, and giving up on the first attempt would turn a
  // momentary lock into a lost save.
  static const int delaysMs[] = {0, 30, 60, 120, 240};
  unsigned long error = 0;
  for (const int delay : delaysMs) {
    if (delay > 0) {
      ::Sleep(delay);
    }
    if (MoveFileExW((WCHAR*) from.utf16(), (WCHAR*) to.utf16(), MOVEFILE_REPLACE_EXISTING) != 0) {
      if (errorMessage) {
        errorMessage->clear();
      }
      return true;
    }
    // Read immediately, before anything else can reset it. Building the message
    // here instead would be a bug: Qt's translation and string formatting can
    // clear the thread's last-error value.
    error = ::GetLastError();
    if ((error != ERROR_SHARING_VIOLATION) && (error != ERROR_LOCK_VIOLATION) && (error != ERROR_ACCESS_DENIED)) {
      break;
    }
  }
  if (errorMessage) {
    *errorMessage = systemErrorString(error);
  }
  return false;
#else
  if (rename(QFile::encodeName(from).data(), QFile::encodeName(to).data()) == 0) {
    if (errorMessage) {
      errorMessage->clear();
    }
    return true;
  }
  const int error = errno;
  if (errorMessage) {
    *errorMessage = systemErrorString(static_cast<unsigned long>(error));
  }
  return false;
#endif
}

QString Utils::lastSystemErrorString() {
#ifdef Q_OS_WIN
  return systemErrorString(::GetLastError());
#else
  return systemErrorString(static_cast<unsigned long>(errno));
#endif
}

QString Utils::systemErrorString(const unsigned long code) {
#ifdef Q_OS_WIN
  const DWORD error = static_cast<DWORD>(code);
  if (error == ERROR_SUCCESS) {
    return QString();
  }
  wchar_t* buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  QString message;
  if (length && buffer) {
    message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
  }
  if (buffer) {
    ::LocalFree(buffer);
  }
  return message.isEmpty() ? QString::fromLatin1("error %1").arg(error)
                           : QString::fromLatin1("%1 [%2]").arg(message).arg(error);
#else
  const int error = static_cast<int>(code);
  if (error == 0) {
    return QString();
  }
  return QString::fromLatin1("%1 [%2]").arg(QString::fromLocal8Bit(std::strerror(error))).arg(error);
#endif
}

QString Utils::richTextForLink(const QString& label, const QString& target) {
  return QString::fromLatin1(
             "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\""
             "\"http://www.w3.org/TR/REC-html40/strict.dtd\">"
             "<html><head><meta name=\"qrichtext\" content=\"1\" />"
             "</head><body><p style=\"margin-top:0px; margin-bottom:0px;"
             "margin-left:0px; margin-right:0px; -qt-block-indent:0;"
             "text-indent:0px;\"><a href=\"%1\">%2</a></p></body></html>")
      .arg(target.toHtmlEscaped(), label.toHtmlEscaped());
}

void Utils::maybeCreateCacheDir(const QString& outputDir) {
  QDir(outputDir).mkdir(QString::fromLatin1("cache"));

  // QDir::mkdir() returns false if the directory already exists,
  // so to prevent confusion this function return void.
}

QString Utils::outputDirToThumbDir(const QString& outputDir) {
  return outputDir + QLatin1String("/cache/thumbs");
}

std::shared_ptr<ThumbnailPixmapCache> Utils::createThumbnailCache(const QString& outputDir) {
  const QSize maxPixmapSize = ApplicationSettings::getInstance().getThumbnailQuality();
  const QString thumbsCachePath(outputDirToThumbDir(outputDir));
  return std::make_shared<ThumbnailPixmapCache>(thumbsCachePath, maxPixmapSize, 40, 5);
}

QString Utils::qssConvertPxToEm(const QString& stylesheet, const double base, const int precision) {
  QString result = "";
  const QRegularExpression pxToEm(R"((\d+(\.\d+)?)px)");

  QRegularExpressionMatchIterator iter = pxToEm.globalMatch(stylesheet);
  int prevIndex = 0;
  while (iter.hasNext()) {
    QRegularExpressionMatch match = iter.next();
    result.append(stylesheet.mid(prevIndex, match.capturedStart() - prevIndex));

    double value = match.captured(1).toDouble();
    value /= base;
    result.append(QString::number(value, 'f', precision)).append("em");

    prevIndex = match.capturedEnd();
  }
  result.append(stylesheet.mid(prevIndex));
  return result;
}
}  // namespace core
