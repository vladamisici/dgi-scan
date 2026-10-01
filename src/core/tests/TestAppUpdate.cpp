// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <AppUpdate.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <boost/test/unit_test.hpp>

using core::AppUpdate;
using core::ReleaseVersion;

namespace Tests {
BOOST_AUTO_TEST_SUITE(AppUpdateTestSuite)

BOOST_AUTO_TEST_CASE(release_names_round_trip) {
  const ReleaseVersion version(ReleaseVersion::fromString(QStringLiteral("1.0.16-dgi.12")));
  BOOST_CHECK_EQUAL(version.major, 1);
  BOOST_CHECK_EQUAL(version.minor, 0);
  BOOST_CHECK_EQUAL(version.patch, 16);
  BOOST_CHECK_EQUAL(version.build, 12);
  BOOST_CHECK_EQUAL(version.toString().toStdString(), "1.0.16-dgi.12");

  // The tag, and the plain version the source carries.
  BOOST_CHECK(ReleaseVersion::fromString(QStringLiteral("v1.0.16-dgi.12")) == version);
  BOOST_CHECK_EQUAL(ReleaseVersion::fromString(QStringLiteral("1.0.16")).build, 0);

  BOOST_CHECK(ReleaseVersion::fromString(QStringLiteral("1.0")).isNull());
  BOOST_CHECK(ReleaseVersion::fromString(QStringLiteral("scantailor-dgi-1.0.16-win64")).isNull());
  BOOST_CHECK(ReleaseVersion::fromString(QString()).isNull());
}

// The release number counts after the version, and numerically: dgi.10 is newer than dgi.9.
BOOST_AUTO_TEST_CASE(releases_are_ordered) {
  const auto v = [](const char* text) { return ReleaseVersion::fromString(QString::fromLatin1(text)); };
  BOOST_CHECK(v("1.0.16-dgi.11") < v("1.0.16-dgi.12"));
  BOOST_CHECK(v("1.0.16-dgi.9") < v("1.0.16-dgi.10"));
  BOOST_CHECK(v("1.0.16-dgi.40") < v("1.0.17-dgi.1"));
  BOOST_CHECK(v("1.0.17-dgi.1") < v("1.1.0-dgi.1"));
  BOOST_CHECK(!(v("1.0.16-dgi.12") < v("1.0.16-dgi.12")));
  BOOST_CHECK(v("1.0.16-dgi.12") != v("1.0.16-dgi.13"));
}

BOOST_AUTO_TEST_CASE(this_build_knows_its_release) {
  const ReleaseVersion current(AppUpdate::current());
  BOOST_CHECK(!current.isNull());
  BOOST_CHECK(current.build >= 0);
}

#ifdef Q_OS_WIN
namespace {
QString systemFile(const char* name) {
  return QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/") + QLatin1String(name));
}
}  // namespace

BOOST_AUTO_TEST_CASE(product_version_is_read_from_the_file) {
  const ReleaseVersion kernel(AppUpdate::productVersion(systemFile("kernel32.dll")));
  BOOST_CHECK(!kernel.isNull());
  BOOST_CHECK(kernel.major >= 6);

  QTemporaryDir dir;
  BOOST_REQUIRE(dir.isValid());
  const QString plain = dir.filePath(QStringLiteral("plain.exe"));
  QFile file(plain);
  BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
  file.write("not a program");
  file.close();
  BOOST_CHECK(AppUpdate::productVersion(plain).isNull());
  BOOST_CHECK(AppUpdate::productVersion(dir.filePath(QStringLiteral("missing.exe"))).isNull());
}

// Every installer in the folder counts, by its version rather than its name,
// and one without a version is passed over.
BOOST_AUTO_TEST_CASE(newest_installer_is_found_by_version) {
  QTemporaryDir dir;
  BOOST_REQUIRE(dir.isValid());
  const QString versioned = dir.filePath(QStringLiteral("scantailor-dgi-1.0.16-win64.exe"));
  BOOST_REQUIRE(QFile::copy(systemFile("kernel32.dll"), versioned));
  const QString unversioned = dir.filePath(QStringLiteral("scantailor-dgi-9.9.9-win64.exe"));
  {
    QFile file(unversioned);
    BOOST_REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("an installer from before releases carried their version");
  }
  BOOST_REQUIRE(QFile::copy(systemFile("kernel32.dll"), dir.filePath(QStringLiteral("something-else.exe"))));

  const AppUpdate::Installer inFolder(AppUpdate::newestInstaller(dir.path()));
  BOOST_REQUIRE(!inFolder.isNull());
  BOOST_CHECK_EQUAL(QFileInfo(inFolder.path).fileName().toStdString(), "scantailor-dgi-1.0.16-win64.exe");
  BOOST_CHECK(inFolder.version == AppUpdate::productVersion(systemFile("kernel32.dll")));

  // The installer itself, as the location.
  const AppUpdate::Installer named(AppUpdate::newestInstaller(versioned));
  BOOST_CHECK_EQUAL(named.path.toStdString(), QFileInfo(versioned).absoluteFilePath().toStdString());
  BOOST_CHECK(AppUpdate::newestInstaller(unversioned).isNull());
  BOOST_CHECK(AppUpdate::newestInstaller(dir.filePath(QStringLiteral("nowhere"))).isNull());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
