// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <AtomicFileOverwriter.h>

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>
#include <boost/test/unit_test.hpp>

namespace Tests {
namespace {
QByteArray readAll(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QByteArray();
  }
  return file.readAll();
}

void writeFile(const QString& path, const QByteArray& contents) {
  QFile file(path);
  BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
  BOOST_REQUIRE(file.write(contents) == contents.size());
}

/**
 * Overwrites \p target through \p spelling - the same file, written the way a
 * caller might spell it - and checks the save took the atomic route and left
 * nothing behind in the directory.
 */
void checkOverwrite(const QString& dirPath, const QString& target, const QString& spelling) {
  writeFile(target, "old");

  AtomicFileOverwriter overwriter;
  QIODevice* const device = overwriter.startWriting(spelling);
  BOOST_REQUIRE_MESSAGE(device, overwriter.errorString().toStdString());
  BOOST_REQUIRE(device->write("new") == 3);
  BOOST_CHECK_MESSAGE(overwriter.commit(), overwriter.errorString().toStdString());

  BOOST_CHECK(readAll(target) == "new");
  BOOST_CHECK_EQUAL(QDir(dirPath).entryList(QDir::Files).size(), 1);
}

#ifdef Q_OS_WIN
/**
 * \p localPath as seen through the drive's administrative share, or an empty
 * string if that share is not reachable from here.
 */
QString adminSharePath(const QString& localPath) {
  const QString native = QDir::toNativeSeparators(localPath);
  if ((native.size() < 3) || (native[1] != QLatin1Char(':'))) {
    return QString();
  }
  const QString unc = QStringLiteral("\\\\localhost\\") + native[0] + QStringLiteral("$") + native.mid(2);
  return QDir(unc).exists() ? unc : QString();
}
#endif
}  // namespace

BOOST_AUTO_TEST_SUITE(AtomicFileOverwriterTestSuite)

BOOST_AUTO_TEST_CASE(test_overwrites_local_path_in_either_spelling) {
  QTemporaryDir dir;
  BOOST_REQUIRE(dir.isValid());
  const QString target = dir.filePath(QStringLiteral("project.ScanTailor"));

  checkOverwrite(dir.path(), target, target);
  checkOverwrite(dir.path(), target, QDir::toNativeSeparators(target));
}

BOOST_AUTO_TEST_CASE(test_abort_leaves_target_and_directory_untouched) {
  QTemporaryDir dir;
  BOOST_REQUIRE(dir.isValid());
  const QString target = dir.filePath(QStringLiteral("project.ScanTailor"));
  writeFile(target, "old");

  AtomicFileOverwriter overwriter;
  QIODevice* const device = overwriter.startWriting(target);
  BOOST_REQUIRE(device);
  device->write("new");
  overwriter.abort();

  BOOST_CHECK(readAll(target) == "old");
  BOOST_CHECK_EQUAL(QDir(dir.path()).entryList(QDir::Files).size(), 1);
}

#ifdef Q_OS_WIN
// A project on a network share is opened as "\\server\share\..." or as
// "//server/share/..." depending on the dialog it came from. QTemporaryFile
// failed on both - it could not create the first and misreported the name of
// the second as "UNC/server/...", so the rename failed and the temporary file
// stayed on the share. Every save there fell back to the unprotected in-place
// write. The administrative share of the local drive stands in for a server.
BOOST_AUTO_TEST_CASE(test_overwrites_unc_path_in_either_spelling) {
  QTemporaryDir dir;
  BOOST_REQUIRE(dir.isValid());
  const QString uncDir = adminSharePath(dir.path());
  if (uncDir.isEmpty()) {
    BOOST_TEST_MESSAGE("The administrative share is not reachable here; skipping the UNC check.");
    return;
  }
  const QString target = dir.filePath(QStringLiteral("project.ScanTailor"));
  const QString uncTarget = uncDir + QStringLiteral("\\project.ScanTailor");

  checkOverwrite(dir.path(), target, uncTarget);
  checkOverwrite(dir.path(), target, QDir::fromNativeSeparators(uncTarget));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
