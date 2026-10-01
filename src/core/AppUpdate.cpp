// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "AppUpdate.h"

#include <config.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <vector>

#include "Diagnostics.h"
#include "version.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace core {
QString ReleaseVersion::toString() const {
  return QStringLiteral("%1.%2.%3-dgi.%4").arg(major).arg(minor).arg(patch).arg(build);
}

ReleaseVersion ReleaseVersion::fromString(const QString& text) {
  static const QRegularExpression pattern(QStringLiteral("^v?(\\d+)\\.(\\d+)\\.(\\d+)(?:-dgi\\.(\\d+))?$"));
  const QRegularExpressionMatch match(pattern.match(text.trimmed()));
  ReleaseVersion version;
  if (match.hasMatch()) {
    version.major = match.captured(1).toInt();
    version.minor = match.captured(2).toInt();
    version.patch = match.captured(3).toInt();
    version.build = match.captured(4).toInt();
  }
  return version;
}

ReleaseVersion AppUpdate::current() {
  ReleaseVersion version(ReleaseVersion::fromString(QString::fromLatin1(VERSION)));
  version.build = DGI_BUILD;
  return version;
}

ReleaseVersion AppUpdate::productVersion(const QString& path) {
  ReleaseVersion version;
#ifdef Q_OS_WIN
  const std::wstring nativePath(QDir::toNativeSeparators(path).toStdWString());
  DWORD handle = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(nativePath.c_str(), &handle);
  if (size == 0) {
    return version;
  }
  std::vector<BYTE> data(size);
  if (!::GetFileVersionInfoW(nativePath.c_str(), 0, size, data.data())) {
    return version;
  }
  VS_FIXEDFILEINFO* info = nullptr;
  UINT length = 0;
  if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info
      || (length < sizeof(VS_FIXEDFILEINFO)) || (info->dwSignature != 0xFEEF04BD)) {
    return version;
  }
  version.major = int(HIWORD(info->dwProductVersionMS));
  version.minor = int(LOWORD(info->dwProductVersionMS));
  version.patch = int(HIWORD(info->dwProductVersionLS));
  version.build = int(LOWORD(info->dwProductVersionLS));
#else
  (void) path;
#endif
  return version;
}

AppUpdate::Installer AppUpdate::newestInstaller(const QString& location, const int settleSeconds) {
  DIAG_SCOPE(diagScope, "update.find");
  QFileInfoList candidates;
  const QFileInfo info(location);
  if (info.isFile()) {
    candidates << info;
  } else if (info.isDir()) {
    candidates = QDir(location).entryInfoList({QStringLiteral("scantailor-dgi-*-win64.exe")},
                                              QDir::Files | QDir::Readable);
  }

  const QDateTime settled = QDateTime::currentDateTimeUtc().addSecs(-settleSeconds);
  Installer newest;
  int unsettled = 0;
  for (const QFileInfo& candidate : candidates) {
    if (candidate.lastModified().toUTC() > settled) {
      ++unsettled;
      continue;
    }
    const ReleaseVersion version(productVersion(candidate.absoluteFilePath()));
    if (!version.isNull() && (newest.isNull() || (newest.version < version))) {
      newest.path = candidate.absoluteFilePath();
      newest.version = version;
    }
  }
  diagScope.attr(diag::Attr("candidates", static_cast<int>(candidates.size())));
  diagScope.attr(diag::Attr("unsettled", unsettled));
  diagScope.attr(diag::Attr("found", !newest.isNull()));
  return newest;
}
}  // namespace core
