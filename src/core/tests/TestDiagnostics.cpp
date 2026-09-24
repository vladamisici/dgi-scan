// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <Diagnostics.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <boost/test/unit_test.hpp>
#include <thread>
#include <vector>

using namespace core;

namespace Tests {
namespace {
std::vector<QJsonObject> readRecords(const QString& path) {
  std::vector<QJsonObject> records;
  QFile file(path);
  BOOST_REQUIRE(file.open(QIODevice::ReadOnly));
  while (!file.atEnd()) {
    const QByteArray line = file.readLine().trimmed();
    if (line.isEmpty()) {
      continue;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    BOOST_REQUIRE_MESSAGE(doc.isObject(), "not a JSON object: " << line.constData());
    records.push_back(doc.object());
  }
  return records;
}

std::vector<QJsonObject> ofType(const std::vector<QJsonObject>& records, const char* type) {
  std::vector<QJsonObject> result;
  for (const QJsonObject& r : records) {
    if (r.value("ev").toString() == QLatin1String(type)) {
      result.push_back(r);
    }
  }
  return result;
}

/** Runs the event loop of the calling thread for \p ms, so the heartbeat can beat. */
void spin(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(2);
  }
}

struct DiagnosticsFixture {
  DiagnosticsFixture() : argc(1), app(argc, argv) {}

  char arg0[16] = "core_tests";
  char* argv[2] = {arg0, nullptr};
  int argc;
  QCoreApplication app;
  QTemporaryDir dir;
};
}  // namespace

BOOST_AUTO_TEST_SUITE(DiagnosticsTestSuite)

BOOST_AUTO_TEST_CASE(test_parse_level) {
  BOOST_CHECK(diag::parseLevel("off", diag::Level::Basic) == diag::Level::Off);
  BOOST_CHECK(diag::parseLevel(" Verbose ", diag::Level::Basic) == diag::Level::Verbose);
  BOOST_CHECK(diag::parseLevel("basic", diag::Level::Off) == diag::Level::Basic);
  BOOST_CHECK(diag::parseLevel("nonsense", diag::Level::Basic) == diag::Level::Basic);
}

BOOST_FIXTURE_TEST_CASE(test_records_and_stall, DiagnosticsFixture) {
  BOOST_REQUIRE(dir.isValid());
  diag::Config config;
  config.level = diag::Level::Verbose;
  config.logDir = dir.path();
  config.stallThresholdMs = 250;
  config.stackCaptureMs = 400;
  config.sampleIntervalMs = 100;
  diag::start(config);
  BOOST_REQUIRE(diag::verbose());

  {
    DIAG_SCOPE(outer, "test.outer");
    outer.attr(diag::Attr("label", QStringLiteral("a \"quoted\" \\ value")));
    DIAG_SCOPE(inner, "test.inner");
    inner.attr(diag::Attr("n", 42));
  }
  for (int i = 0; i < 5; ++i) {
    DIAG_COUNT("test.counter");
  }
  std::thread worker([]() {
    diag::setThreadRole("worker");
    diag::beginTask();
    {
      DIAG_SCOPE(taskScope, "test.task");
    }
    diag::endTask();
  });
  worker.join();

  spin(300);
  {
    // The GUI thread is blocked inside an operation: the watchdog must report a
    // stall naming it, with a stack.
    DIAG_SCOPE(blocking, "test.blocking");
    QThread::msleep(900);
  }
  spin(400);
  diag::sampleNow("test", {diag::Attr("cycle", 1)});
  const QString path = diag::logFilePath();
  diag::stop();
  BOOST_CHECK(!diag::enabled());

  const std::vector<QJsonObject> records = readRecords(path);
  BOOST_REQUIRE(!records.empty());
  BOOST_CHECK_EQUAL(records.front().value("ev").toString().toStdString(), "start");
  BOOST_CHECK_EQUAL(records.back().value("ev").toString().toStdString(), "stop");

  bool sawOuter = false;
  bool sawInner = false;
  bool sawTask = false;
  for (const QJsonObject& op : ofType(records, "op")) {
    const QString name = op.value("name").toString();
    if (name == QLatin1String("test.outer")) {
      sawOuter = true;
      BOOST_CHECK_EQUAL(op.value("depth").toInt(), 1);
      BOOST_CHECK_EQUAL(op.value("label").toString().toStdString(), "a \"quoted\" \\ value");
      BOOST_CHECK_EQUAL(op.value("th").toString().toStdString(), "gui");
    } else if (name == QLatin1String("test.inner")) {
      sawInner = true;
      BOOST_CHECK_EQUAL(op.value("depth").toInt(), 2);
      BOOST_CHECK_EQUAL(op.value("n").toInt(), 42);
    } else if (name == QLatin1String("test.task")) {
      sawTask = true;
      BOOST_CHECK_EQUAL(op.value("th").toString().toStdString(), "worker");
      BOOST_CHECK(op.value("task").toInt() > 0);
    }
  }
  BOOST_CHECK(sawOuter);
  BOOST_CHECK(sawInner);
  BOOST_CHECK(sawTask);

  int counterCalls = 0;
  for (const QJsonObject& agg : ofType(records, "agg")) {
    if (agg.value("name").toString() == QLatin1String("test.counter")) {
      counterCalls += agg.value("n").toInt();
    }
  }
  BOOST_CHECK_EQUAL(counterCalls, 5);

  const std::vector<QJsonObject> stalls = ofType(records, "stall");
  BOOST_REQUIRE_EQUAL(stalls.size(), 1u);
  const QJsonObject stall = stalls.front();
  BOOST_CHECK(stall.value("dur").toDouble() >= 800);
  BOOST_CHECK(stall.value("dur").toDouble() < 1500);
  BOOST_CHECK_EQUAL(stall.value("ops").toArray().at(0).toString().toStdString(), "test.blocking");
#if defined(_M_X64)
  BOOST_CHECK(!stall.value("stack").toArray().isEmpty());
  // Once the stack is captured, the stall is also reported while still going,
  // under the same "at", so a process killed mid-stall still leaves its stack.
  bool sawProgress = false;
  for (const QJsonObject& progress : ofType(records, "stall_progress")) {
    if (progress.value("at").toDouble() == stall.value("at").toDouble()) {
      sawProgress = !progress.value("stack").toArray().isEmpty();
    }
  }
  BOOST_CHECK(sawProgress);
#endif
  BOOST_CHECK_EQUAL(stall.value("phase").toString().toStdString(), "startup");

  bool sawCycleSample = false;
  for (const QJsonObject& res : ofType(records, "res")) {
    BOOST_CHECK(res.contains("private_mb"));
    if (res.value("reason").toString() == QLatin1String("test")) {
      sawCycleSample = (res.value("cycle").toInt() == 1);
    }
  }
  BOOST_CHECK(sawCycleSample);
  BOOST_CHECK(!ofType(records, "beat").empty());
}

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
