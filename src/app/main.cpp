// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <config.h>
#include <core/Application.h>
#include <core/ApplicationSettings.h>
#include <core/ColorSchemeFactory.h>
#include <core/ColorSchemeManager.h>
#include <core/CrashHandler.h>
#include <core/Diagnostics.h>
#include <core/FontIconPack.h>
#include <core/IconProvider.h>
#include <core/StyledIconPack.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MainWindow.h"
#include "StressDriver.h"

namespace {
bool hasStressFlag(const int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--stress") == 0) {
      return true;
    }
  }
  return false;
}

/**
 * Isolates a stress run from the operator's own installation: its settings,
 * recent-projects list and recovery files live under the run's work folder and
 * a per-instance application name, never in the operator's profile. The
 * operator's settings can be copied in first, so the run reflects how their
 * copy is configured.
 */
void isolateStressRun(const StressDriver::Config& config) {
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QDir(config.workDir).absoluteFilePath(QStringLiteral("settings")));
  const QString iniPath = QSettings().fileName();
  // Always from scratch: a work folder reused from an earlier run must not carry
  // that run's settings into this one.
  QFile::remove(iniPath);
  if (!config.operatorSettingsFile.isEmpty() && QFileInfo::exists(config.operatorSettingsFile)) {
    QDir().mkpath(QFileInfo(iniPath).absolutePath());
    QFile::copy(config.operatorSettingsFile, iniPath);
  }

  QSettings settings;
  for (auto it = config.settings.constBegin(); it != config.settings.constEnd(); ++it) {
    settings.setValue(QStringLiteral("settings/") + it.key(), it.value());
  }
  // The only question a programmatic page change can raise.
  settings.setValue(QStringLiteral("settings/selection_canceling_question"), false);
  // Recent projects and the last-used folders are the operator's, even when the
  // settings file came from them.
  settings.remove(QStringLiteral("project"));
  settings.sync();
}

/** Records the settings that bear on performance, so a report can tell two PCs' configurations apart. */
void logSettings() {
  ApplicationSettings& app = ApplicationSettings::getInstance();
  const QSettings settings;
  const QSize thumbQuality = app.getThumbnailQuality();
  const QSizeF thumbSize = app.getMaxLogicalThumbnailSize();
  // The same clamp WorkerThreadPool applies: a copied INI can ask for more
  // threads than this PC has, and the report must show what actually ran.
  int maxThreads = QThread::idealThreadCount();
  if (sizeof(void*) <= 4) {
    maxThreads = std::min(maxThreads, 2);
  }
  const int storedThreads = settings.value("settings/batch_processing_threads", maxThreads).toInt();
  core::diag::event(
      "settings",
      {core::diag::Attr("auto_save_project", app.isAutoSaveProjectEnabled()),
       core::diag::Attr("auto_save_interval_sec", app.getAutoSaveIntervalSec()),
       core::diag::Attr("crash_recovery", app.isCrashRecoveryEnabled()),
       core::diag::Attr("opengl", app.isOpenGlEnabled()),
       core::diag::Attr("thumbnail_quality_w", thumbQuality.width()),
       core::diag::Attr("thumbnail_quality_h", thumbQuality.height()),
       core::diag::Attr("max_logical_thumb_w", thumbSize.width()),
       core::diag::Attr("max_logical_thumb_h", thumbSize.height()),
       core::diag::Attr("batch_threads", std::min(storedThreads, maxThreads)),
       core::diag::Attr("batch_threads_setting", storedThreads),
       core::diag::Attr("worker_thread_priority",
                        settings.value("settings/worker_thread_priority").toString() == QLatin1String("low")
                            ? "low"
                            : "normal"),
       core::diag::Attr("highlight_deviation", app.isHighlightDeviationEnabled()),
       core::diag::Attr("single_column_thumbs", app.isSingleColumnThumbnailDisplayEnabled()),
       core::diag::Attr("color_scheme", app.getColorScheme()),
       core::diag::Attr("tiff_bw_compression", app.getTiffBwCompression()),
       core::diag::Attr("tiff_color_compression", app.getTiffColorCompression()),
       core::diag::Attr("settings_file", settings.fileName())});
}
}  // namespace

int main(int argc, char* argv[]) {
  const bool stress = hasStressFlag(argc, argv);

  QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
  if (stress) {
    // A native file dialog is invisible to Qt, so an unattended run could not
    // notice one, let alone dismiss it.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  }

  Application app(argc, argv);

#ifdef _WIN32
  // Get rid of all references to Qt's installation directory.
  Application::setLibraryPaths(QStringList(Application::applicationDirPath()));
#endif

  QStringList args = Application::arguments();

  StressDriver::Config stressConfig;
  if (stress) {
    const int flag = args.indexOf(QStringLiteral("--stress"));
    QString error = QStringLiteral("missing configuration file after --stress");
    if ((flag + 1 >= args.size()) || !StressDriver::loadConfig(args.at(flag + 1), &stressConfig, &error)) {
      std::fprintf(stderr, "scantailor --stress: %s\n", error.toLocal8Bit().constData());
      return StressDriver::EXIT_FATAL;
    }
  }

  // This information is used by QSettings.
  Application::setApplicationName(stress ? QStringLiteral(APPLICATION_NAME "-stress-%1").arg(stressConfig.instance)
                                         : QStringLiteral(APPLICATION_NAME));
  Application::setOrganizationName(ORGANIZATION_NAME);

  QSettings::setDefaultFormat(QSettings::IniFormat);
  if (stress) {
    isolateStressRun(stressConfig);
  } else if (app.isPortableVersion()) {
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, app.getPortableConfigPath());
  }
  QSettings settings;

  // Installed before any project is touched. Until now a crash - most often an
  // exception escaping a worker thread - killed the process without a dialog,
  // without a log line and without a dump, which is why "it just closes" reports
  // were impossible to act on. From here on every such death leaves a minidump
  // and a log entry behind.
  QString reportDir;
  if (stress) {
    reportDir = QDir(stressConfig.workDir).absoluteFilePath(QStringLiteral("logs"));
  } else if (app.isPortableVersion()) {
    reportDir = app.getPortableConfigPath() + QLatin1String("/crashes");
  } else {
    reportDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QLatin1String("/crashes");
  }
  core::CrashHandler::install(reportDir);

  // Everyday use gets the basic level unless the INI or the SCANTAILOR_DIAG
  // environment variable says otherwise: it is what lets a slow PC be diagnosed
  // from a normal day's work instead of a guess.
  core::diag::Config diagConfig;
  diagConfig.logDir = core::CrashHandler::reportDir();
  if (stress) {
    diagConfig.level = core::diag::Level::Verbose;
    // A test run exists to explain every stall, so it takes the stack of any
    // stall worth reporting, and a dump of any that is really long.
    diagConfig.stackCaptureMs = 300;
    diagConfig.dumpStallMs = 15000;
  } else {
    diagConfig.level
        = core::diag::parseLevel(settings.value(QStringLiteral("diagnostics/level")).toString(), core::diag::Level::Basic);
  }
  core::diag::start(diagConfig);

  app.installLanguage(ApplicationSettings::getInstance().getLanguage());
  logSettings();

  {
    std::unique_ptr<ColorScheme> scheme
        = ColorSchemeFactory().create(ApplicationSettings::getInstance().getColorScheme());
    ColorSchemeManager::instance().setColorScheme(*scheme);
  }
  IconProvider::getInstance().setIconPack(StyledIconPack::createDefault());

  auto* mainWnd = new MainWindow();
  mainWnd->setAttribute(Qt::WA_DeleteOnClose);
  if (stress || (settings.value("mainWindow/maximized") == false)) {
    mainWnd->show();
  } else {
    // mainWnd->showMaximized();  // Doesn't work for Windows.
    QTimer::singleShot(0, mainWnd, &QMainWindow::showMaximized);
  }

  // Runs once the event loop has started: until then the GUI thread is building
  // the window, and nobody is waiting on it yet.
  QTimer::singleShot(0, []() { core::diag::setPhase("run"); });

  if (stress) {
    auto* driver = new StressDriver(mainWnd, stressConfig);
    driver->setParent(&app);
    // Give the window time to appear before the workload starts.
    QTimer::singleShot(500, driver, &StressDriver::run);
  } else if (args.size() > 1) {
    // The operator double-clicked a title and is now waiting for it: this is not
    // start-up any more, whatever the event loop's state.
    core::diag::setPhase("run");
    mainWnd->openProject(args.at(1));
  } else {
    // Only when no project was named on the command line. openProject() is not
    // synchronous - it can put up the Fix DPI dialog and finish later - so
    // testing "is a project loaded yet" here would not be a reliable guard.
    mainWnd->offerUnsavedSessionRecovery();
  }

  const int result = Application::exec();
  core::diag::stop();
  return stress ? StressDriver::exitCode() : result;
}  // main
