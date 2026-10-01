// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <ProjectRecovery.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <boost/test/unit_test.hpp>

using core::ProjectRecovery;

namespace Tests {
namespace {
struct RecoveryFixture {
  RecoveryFixture() {
    QCoreApplication::setApplicationName(QStringLiteral("core_tests"));
    QStandardPaths::setTestModeEnabled(true);
    QDir(QFileInfo(ProjectRecovery::unsavedSessionPath()).absolutePath()).removeRecursively();
  }

  ~RecoveryFixture() {
    QDir(QFileInfo(ProjectRecovery::unsavedSessionPath()).absolutePath()).removeRecursively();
    QStandardPaths::setTestModeEnabled(false);
  }
};
}  // namespace

BOOST_FIXTURE_TEST_SUITE(ProjectRecoveryTestSuite, RecoveryFixture)

BOOST_AUTO_TEST_CASE(test_unsaved_session_path_is_recognised) {
  const QString path = ProjectRecovery::unsavedSessionPath();
  BOOST_REQUIRE(!path.isEmpty());
  BOOST_CHECK(ProjectRecovery::isUnsavedSessionPath(path));
  BOOST_CHECK(ProjectRecovery::isUnsavedSessionPath(QDir::toNativeSeparators(path)));
#ifdef Q_OS_WIN
  BOOST_CHECK(ProjectRecovery::isUnsavedSessionPath(path.toUpper()));
#endif
  BOOST_CHECK(!ProjectRecovery::isUnsavedSessionPath(path + QLatin1String(".bak")));
  BOOST_CHECK(!ProjectRecovery::isUnsavedSessionPath(QString()));
}

// A second holder - standing in for a second running copy of the application -
// must be refused while the first is alive, and admitted once it lets go.
BOOST_AUTO_TEST_CASE(test_claim_is_exclusive_until_released) {
  std::unique_ptr<QLockFile> first = ProjectRecovery::claimUnsavedSession();
  BOOST_REQUIRE(first);
  BOOST_CHECK(!ProjectRecovery::claimUnsavedSession());

  first.reset();
  BOOST_CHECK(ProjectRecovery::claimUnsavedSession());
}

namespace {
void writeProject(const QString& path, const QStringList& fileNames) {
  QString xml = QStringLiteral(
      "<project outputDirectory=\"D:/out\" version=\"3\"><directories><directory id=\"1\" path=\"D:/in\"/>"
      "</directories><files>");
  int id = 2;
  for (const QString& name : fileNames) {
    xml += QStringLiteral("<file id=\"%1\" dirId=\"1\" name=\"%2\"/>").arg(id++).arg(name);
  }
  xml += QStringLiteral("</files><images/><pages/><filters/></project>");
  QFile file(path);
  BOOST_REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(xml.toUtf8());
}
}  // namespace

// A folder copied from another title, leftovers and all, must not have that
// title's snapshot offered as this project's work.
BOOST_AUTO_TEST_CASE(test_snapshot_of_another_title_is_told_apart) {
  QTemporaryDir dir;
  BOOST_REQUIRE(dir.isValid());
  const QString project = dir.filePath(QStringLiteral("project.ScanTailor"));
  const QString snapshot = ProjectRecovery::snapshotPathFor(project);

  writeProject(project, {QStringLiteral("0001_2161.tif"), QStringLiteral("0002_2161.tif")});
  writeProject(snapshot, {QStringLiteral("0001_1302.tif"), QStringLiteral("0002_1302.tif")});
  BOOST_CHECK(!ProjectRecovery::snapshotMatchesProject(project));

  // The same title with pages added or removed since the last save.
  writeProject(snapshot, {QStringLiteral("0002_2161.TIF"), QStringLiteral("0003_2161.tif")});
  BOOST_CHECK(ProjectRecovery::snapshotMatchesProject(project));

  // Nothing to compare: given the benefit of the doubt.
  writeProject(snapshot, {});
  BOOST_CHECK(ProjectRecovery::snapshotMatchesProject(project));
  QFile::remove(project);
  writeProject(snapshot, {QStringLiteral("0001_1302.tif")});
  BOOST_CHECK(ProjectRecovery::snapshotMatchesProject(project));
}

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
