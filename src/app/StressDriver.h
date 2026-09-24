// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_STRESSDRIVER_H_
#define SCANTAILOR_APP_STRESSDRIVER_H_

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <functional>

#include "PageId.h"

class MainWindow;

/**
 * \brief Works the real main window through a scripted, repeatable workload.
 *
 * Started with `scantailor --stress <config.json>`. It does what an operator
 * does - create a project from a folder of scans, batch-process every stage,
 * step through pages in each stage, let autosave run, save, close, reopen - for
 * a number of cycles, while core::diag records how long everything takes, every
 * stall of the GUI thread, and the process's resources after each cycle. That is
 * what the diagnostics suite (diagnostics/Run-StressTest.ps1) analyses.
 *
 * It never shows a dialog and never waits on one: anything modal that appears
 * anyway is recorded as a failure and dismissed, so an unattended run cannot
 * hang on a prompt.
 *
 * The configuration format is documented in diagnostics/README.md; the records
 * it writes in diagnostics/SCHEMA.md.
 */
class StressDriver : public QObject {
  Q_OBJECT
 public:
  struct Config {
    int instance = 1;
    QString workDir;
    QString scansDir;
    int maxPages = 40;
    int syntheticCount = 0;
    QString syntheticKind = QStringLiteral("gray");
    int syntheticDpi = 300;
    int syntheticSpreadEvery = 0;
    int cycles = 5;
    QString mode = QStringLiteral("reopen");
    int navPages = 10;
    QList<int> stages{0, 1, 2, 3, 4, 5};
    int autosavesPerCycle = 2;
    /** In reopen cycles, re-run the Output batch (every page already done) before closing. */
    bool rebatchOutput = true;
    bool thumbScroll = true;
    int settleMs = 3000;
    int dwellMs = 300;
    int batchTimeoutSecPerPage = 180;
    int pageLoadTimeoutSec = 180;
    QString operatorSettingsFile;
    QVariantMap settings;
  };

  enum ExitCode { EXIT_OK = 0, EXIT_FAILURES = 2, EXIT_FATAL = 3 };

  /** \brief Reads the configuration file. On failure, \p error says why. */
  static bool loadConfig(const QString& path, Config* config, QString* error);

  StressDriver(MainWindow* window, const Config& config);

  ~StressDriver() override;

  /** \brief The process exit code for the run, valid once the window has closed. */
  static int exitCode();

 public slots:

  /** \brief Runs the whole scenario, then closes the window. Call once the event loop is running. */
  void run();

 private:
  bool prepareScans();

  bool generateSyntheticScans(const QString& dir);

  bool runCycle(int cycle);

  bool createProject(const QString& outDir);

  bool openProject();

  bool saveProjectAs(const QString& path);

  bool closeProject();

  bool switchStage(int stage);

  bool batchStage(int stage, bool rebatch);

  bool navigateStage(int stage);

  void scrollThumbnails(int stage);

  void runAutosave();

  void settle();

  bool waitFor(const std::function<bool()>& condition, qint64 timeoutMs);

  bool waitIdle(qint64 timeoutMs);

  bool waitForLoad(qint64 timeoutMs);

  void pump(int ms);

  bool isIdle() const;

  /** False once the run is fatal or the window is gone; nothing may touch the window then. */
  bool alive() const;

  int currentStage() const;

  void checkForDialogs();

  void onInteractiveLoadFinished(const PageId& pageId, int filterIdx, int outcome);

  void onBatchFinished(bool completed);

  void onOutOfMemory();

  void failure(const QString& what);

  void fatal(const QString& what);

  void finish();

  QString projectFilePath() const;

  QPointer<MainWindow> m_window;
  Config m_config;
  QStringList m_scans;
  QTimer m_sentinel;
  QElapsedTimer m_runTimer;
  int m_cycle = 0;
  QString m_step;
  int m_failures = 0;
  bool m_fatal = false;
  bool m_loadDone = false;
  int m_loadOutcome = -1;
  bool m_batchDone = false;
  bool m_batchCompleted = false;
  int m_waitDepth = 0;
};


#endif  // ifndef SCANTAILOR_APP_STRESSDRIVER_H_
