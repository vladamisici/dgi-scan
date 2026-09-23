// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <ProjectRecovery.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>
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

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
