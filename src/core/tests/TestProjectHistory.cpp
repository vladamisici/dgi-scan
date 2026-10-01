// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <ProjectHistory.h>

#include <boost/test/unit_test.hpp>

namespace Tests {
BOOST_AUTO_TEST_SUITE(ProjectHistoryTestSuite)

BOOST_AUTO_TEST_CASE(custom_name_and_verification_metadata_survive_touch) {
  const QString projectFile(QLatin1String("C:/projects/book.ScanTailor"));
  core::ProjectHistory history;
  history.touch(projectFile, 42, QLatin1String("C:/output"));
  history.rename(projectFile, QLatin1String("  Quality check  "));
  history.setVerification(projectFile, true,
                          {QLatin1String("C:/originals/one"), QLatin1String("C:/originals/two")});

  // Opening or saving the same project moves it to the top but must not discard
  // the operator's name or its verification inputs.
  history.touch(projectFile);

  BOOST_REQUIRE_EQUAL(history.entries().size(), 1);
  const core::ProjectHistory::Entry& entry = history.entries().front();
  BOOST_CHECK_EQUAL(entry.displayName().toStdString(), "Quality check");
  BOOST_CHECK_EQUAL(entry.pageCount, 42);
  BOOST_CHECK_EQUAL(entry.outputDirectory.toStdString(), "C:/output");
  BOOST_CHECK(entry.verification);
  BOOST_REQUIRE_EQUAL(entry.inputDirectories.size(), 2);
}

BOOST_AUTO_TEST_CASE(normal_mode_clears_verification_inputs) {
  const QString projectFile(QLatin1String("C:/projects/book.ScanTailor"));
  core::ProjectHistory history;
  history.touch(projectFile);
  history.setVerification(projectFile, true, {QLatin1String("C:/originals")});
  history.setVerification(projectFile, false);

  BOOST_REQUIRE_EQUAL(history.entries().size(), 1);
  BOOST_CHECK(!history.entries().front().verification);
  BOOST_CHECK(history.entries().front().inputDirectories.isEmpty());
}

#ifdef Q_OS_WIN
// Every title's project is "project": the folder is what tells them apart.
BOOST_AUTO_TEST_CASE(short_location_names_the_title_folder) {
  using core::ProjectHistory;
  BOOST_CHECK_EQUAL(
      ProjectHistory::shortLocation(QLatin1String("D:/00scanari/2161/00_clean/01_romane/project.ScanTailor"))
          .toStdString(),
      "2161\\00_clean\\01_romane");
  BOOST_CHECK_EQUAL(
      ProjectHistory::shortLocation(QLatin1String("D:\\00scanari\\nou\\00_clean\\01_romane\\project.ScanTailor"))
          .toStdString(),
      "nou\\00_clean\\01_romane");
  // Nothing worth leaving out.
  BOOST_CHECK_EQUAL(ProjectHistory::shortLocation(QLatin1String("D:/scans/book/project.ScanTailor")).toStdString(),
                    "D:\\scans\\book");
  BOOST_CHECK_EQUAL(
      ProjectHistory::shortLocation(QLatin1String("//server1/kit/1302/project.ScanTailor")).toStdString(),
      "\\\\server1\\kit\\1302");
  BOOST_CHECK(ProjectHistory::shortLocation(QString()).isEmpty());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
