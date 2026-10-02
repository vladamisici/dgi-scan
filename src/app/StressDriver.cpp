// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "StressDriver.h"

#include <core/ApplicationSettings.h>
#include <core/CrashHandler.h>
#include <core/Diagnostics.h>
#include <core/ProjectHistory.h>
#include <core/ProjectRecovery.h>
#include <imageproc/BinaryImage.h>
#include <imageproc/Grayscale.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPainter>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <thread>

#include "ImageFileInfo.h"
#include "ImageMetadataLoader.h"
#include "MainWindow.h"
#include "OutOfMemoryHandler.h"
#include "PageSequence.h"
#include "ProcessingTaskQueue.h"
#include "ProjectPages.h"
#include "SmartFilenameOrdering.h"
#include "TiffWriter.h"
#include "WorkerThreadPool.h"

using namespace core;

namespace {
int g_exitCode = StressDriver::EXIT_FATAL;

const char* const STAGE_NAMES[] = {"fix_orientation", "page_split", "deskew", "select_content", "page_layout", "output"};
constexpr int STAGE_COUNT = 6;

const char* stageName(const int stage) {
  return (stage >= 0 && stage < STAGE_COUNT) ? STAGE_NAMES[stage] : "unknown";
}

const char* outcomeName(const int outcome) {
  switch (outcome) {
    case MainWindow::LOAD_OK:
      return "ok";
    case MainWindow::LOAD_ERROR:
      return "load_error";
    case MainWindow::LOAD_TASK_FAILED:
      return "task_failed";
    case MainWindow::LOAD_OUTPUT_NOT_READY:
      return "output_not_ready";
    default:
      return "timeout";
  }
}

bool isImageFile(const QFileInfo& info) {
  static const QStringList suffixes{"tif", "tiff", "png", "jpg", "jpeg"};
  return suffixes.contains(info.suffix().toLower());
}
}  // namespace

/*------------------------------- Configuration -------------------------------*/

bool StressDriver::loadConfig(const QString& path, Config* config, QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    *error = QStringLiteral("cannot open ") + path;
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (!doc.isObject()) {
    *error = QStringLiteral("not a JSON object: ") + parseError.errorString();
    return false;
  }
  const QJsonObject obj = doc.object();
  Config c;
  c.instance = obj.value("instance").toInt(c.instance);
  c.workDir = QDir::fromNativeSeparators(obj.value("workDir").toString());
  c.scansDir = QDir::fromNativeSeparators(obj.value("scansDir").toString());
  c.maxPages = obj.value("maxPages").toInt(c.maxPages);
  const QJsonObject synthetic = obj.value("synthetic").toObject();
  c.syntheticCount = synthetic.value("count").toInt(0);
  c.syntheticKind = synthetic.value("kind").toString(c.syntheticKind);
  c.syntheticDpi = synthetic.value("dpi").toInt(c.syntheticDpi);
  c.syntheticSpreadEvery = synthetic.value("spreadEvery").toInt(0);
  c.cycles = obj.value("cycles").toInt(c.cycles);
  c.mode = obj.value("mode").toString(c.mode);
  c.navPages = obj.value("navPages").toInt(c.navPages);
  if (obj.value("stages").isArray()) {
    c.stages.clear();
    for (const QJsonValue& v : obj.value("stages").toArray()) {
      const int stage = v.toInt(-1);
      if (stage >= 0 && stage < STAGE_COUNT) {
        c.stages.push_back(stage);
      }
    }
  }
  c.autosavesPerCycle = obj.value("autosavesPerCycle").toInt(c.autosavesPerCycle);
  c.rebatchOutput = obj.value("rebatchOutput").toBool(c.rebatchOutput);
  c.thumbScroll = obj.value("thumbScroll").toBool(c.thumbScroll);
  c.settleMs = obj.value("settleMs").toInt(c.settleMs);
  c.dwellMs = obj.value("dwellMs").toInt(c.dwellMs);
  c.batchTimeoutSecPerPage = obj.value("batchTimeoutSecPerPage").toInt(c.batchTimeoutSecPerPage);
  c.pageLoadTimeoutSec = obj.value("pageLoadTimeoutSec").toInt(c.pageLoadTimeoutSec);
  c.operatorSettingsFile = QDir::fromNativeSeparators(obj.value("operatorSettingsFile").toString());
  c.settings = obj.value("settings").toObject().toVariantMap();

  if (c.workDir.isEmpty()) {
    *error = QStringLiteral("workDir is required");
    return false;
  }
  if (c.scansDir.isEmpty() && c.syntheticCount <= 0) {
    *error = QStringLiteral("either scansDir or synthetic.count is required");
    return false;
  }
  if (c.mode != QLatin1String("reopen") && c.mode != QLatin1String("fresh")) {
    *error = QStringLiteral("mode must be \"reopen\" or \"fresh\"");
    return false;
  }
  c.cycles = std::max(1, c.cycles);
  *config = c;
  return true;
}

/*----------------------------------- Setup -----------------------------------*/

StressDriver::StressDriver(MainWindow* window, const Config& config) : m_window(window), m_config(config) {
  connect(window, &MainWindow::interactiveLoadFinished, this, &StressDriver::onInteractiveLoadFinished);
  connect(window, &MainWindow::batchProcessingFinished, this, &StressDriver::onBatchFinished);
  connect(&OutOfMemoryHandler::instance(), &OutOfMemoryHandler::outOfMemory, this, [this]() { onOutOfMemory(); });

  // Any dialog that manages to appear would otherwise wait forever for a click
  // nobody will make. Timers keep firing inside a dialog's own event loop, which
  // is what lets this notice and dismiss it.
  m_sentinel.setInterval(200);
  connect(&m_sentinel, &QTimer::timeout, this, [this]() { checkForDialogs(); });
}

StressDriver::~StressDriver() = default;

int StressDriver::exitCode() {
  return g_exitCode;
}

QString StressDriver::projectFilePath() const {
  return QDir(m_config.workDir).absoluteFilePath(QStringLiteral("title.ScanTailor"));
}

/*------------------------------------ Run ------------------------------------*/

void StressDriver::run() {
  m_runTimer.start();
  m_sentinel.start();

  diag::event("stress.config",
              {diag::Attr("instance", m_config.instance), diag::Attr("cycles", m_config.cycles),
               diag::Attr("mode", m_config.mode), diag::Attr("pages", m_config.maxPages),
               diag::Attr("scans", m_config.scansDir), diag::Attr("work", m_config.workDir),
               diag::Attr("nav_pages", m_config.navPages), diag::Attr("synthetic", m_config.syntheticCount)});
  CrashHandler::log(QStringLiteral("Stress run started: ") + m_config.workDir);

  if (!prepareScans()) {
    finish();
    return;
  }

  for (int cycle = 1; cycle <= m_config.cycles && !m_fatal; ++cycle) {
    if (!runCycle(cycle)) {
      break;
    }
  }
  finish();
}

bool StressDriver::runCycle(const int cycle) {
  m_cycle = cycle;
  diag::event("stress.cycle_begin", {diag::Attr("cycle", cycle)});
  QElapsedTimer timer;
  timer.start();

  const QString outDir = QDir(m_config.workDir).absoluteFilePath(QStringLiteral("out"));
  const bool fresh = (cycle == 1) || (m_config.mode == QLatin1String("fresh"));

  if (fresh) {
    if (cycle > 1) {
      // Otherwise the output stage would find every page already done and the
      // cycle would measure the cache rather than the processing. Named, so a
      // stall it causes is put down to the test, not to the application.
      DIAG_SCOPE(cleanScope, "stress.clean_outputs");
      QDir(outDir).removeRecursively();
    }
    if (!createProject(outDir)) {
      fatal(QStringLiteral("could not create the project"));
      return false;
    }
    for (int stage = 0; stage < STAGE_COUNT && alive(); ++stage) {
      batchStage(stage, false);
    }
    if (!alive()) {
      return false;
    }
    if (!saveProjectAs(projectFilePath())) {
      fatal(QStringLiteral("could not save the project"));
      return false;
    }
  } else if (!openProject()) {
    fatal(QStringLiteral("could not reopen the project"));
    return false;
  }

  for (const int stage : m_config.stages) {
    if (!alive()) {
      return false;
    }
    navigateStage(stage);
  }
  if (m_config.thumbScroll && alive()) {
    scrollThumbnails(currentStage());
  }
  if (!fresh && alive() && m_config.rebatchOutput) {
    // Every output is already on disk, so this is the cost of confirming that -
    // which is what an operator re-running a batch on a finished title pays.
    batchStage(STAGE_COUNT - 1, true);
  }
  if (!alive()) {
    return false;
  }
  runAutosave();
  if (!alive()) {
    return false;
  }

  {
    m_step = QStringLiteral("save");
    QElapsedTimer saveTimer;
    saveTimer.start();
    const bool ok = m_window->saveProjectWithFeedback(m_window->m_projectFile);
    diag::event("stress.step", {diag::Attr("cycle", cycle), diag::Attr("step", "save"),
                                diag::Attr("dur", saveTimer.nsecsElapsed() / 1e6), diag::Attr("ok", ok)});
    if (!ok) {
      failure(QStringLiteral("save failed"));
    }
  }

  closeProject();
  if (!alive()) {
    return false;
  }
  settle();
  diag::event("stress.cycle_end", {diag::Attr("cycle", cycle), diag::Attr("dur", timer.nsecsElapsed() / 1e6)});
  return alive();
}

bool StressDriver::alive() const {
  // The out-of-memory handler deletes the main window; after that, or after any
  // fatal failure, the scenario must not touch it again.
  return !m_fatal && m_window;
}

void StressDriver::finish() {
  m_sentinel.stop();
  if (m_window && m_window->isProjectLoaded()) {
    if (m_window->isBatchProcessingInProgress()) {
      m_window->stopBatchProcessing();
    }
    m_window->releaseUnsavedSession();
    ProjectRecovery::discardSnapshot(m_window->m_projectFile);
    m_window->closeProjectWithoutSaving();
  }

  g_exitCode = m_fatal ? EXIT_FATAL : (m_failures > 0 ? EXIT_FAILURES : EXIT_OK);
  diag::event("stress.finished",
              {diag::Attr("ok", g_exitCode == EXIT_OK), diag::Attr("cycles", m_cycle),
               diag::Attr("failures", m_failures), diag::Attr("dur", m_runTimer.nsecsElapsed() / 1e6),
               diag::Attr("exit_code", g_exitCode)});
  CrashHandler::log(QStringLiteral("Stress run finished with exit code %1").arg(g_exitCode));

  if (m_window) {
    // The application quits when the window has closed.
    m_window->close();
  } else {
    // The window is gone - the out-of-memory handler deletes it - so there is
    // nothing left whose closing would end the run.
    QCoreApplication::exit(g_exitCode);
  }
}

/*----------------------------------- Scans -----------------------------------*/

bool StressDriver::prepareScans() {
  QString dir = m_config.scansDir;
  if (m_config.syntheticCount > 0) {
    dir = QDir(m_config.workDir).absoluteFilePath(QStringLiteral("scans"));
    if (!generateSyntheticScans(dir)) {
      fatal(QStringLiteral("could not generate synthetic scans in ") + dir);
      return false;
    }
  }

  QFileInfoList files;
  {
    // Listing a scans folder on a share can take seconds; named so that the
    // stall is put down to the test.
    DIAG_SCOPE(listScope, "stress.list_scans");
    for (const QFileInfo& info : QDir(dir).entryInfoList(QDir::Files)) {
      if (isImageFile(info)) {
        files.push_back(info);
      }
    }
    std::sort(files.begin(), files.end(), SmartFilenameOrdering());
  }
  if (m_config.maxPages > 0 && files.size() > m_config.maxPages) {
    files.erase(files.begin() + m_config.maxPages, files.end());
  }
  for (const QFileInfo& info : files) {
    m_scans.push_back(info.absoluteFilePath());
  }
  if (m_scans.isEmpty()) {
    fatal(QStringLiteral("no images in ") + dir);
    return false;
  }
  return true;
}

/**
 * Pages with a scanner border, slightly skewed text blocks and, for spreads, a
 * dark fold - enough for every stage to have real work: orientation, splitting,
 * deskew, content detection. Generated from the index, so every run and every
 * machine processes exactly the same images.
 */
bool StressDriver::generateSyntheticScans(const QString& dir) {
  if (!QDir().mkpath(dir)) {
    return false;
  }
  QElapsedTimer timer;
  timer.start();
  const int dpi = std::max(100, m_config.syntheticDpi);
  const int dpm = qRound(dpi / 0.0254);
  const QSize a4(qRound(8.27 * dpi), qRound(11.69 * dpi));

  // Fixed compression (libtiff's LZW = 5, CCITT G4 = 4), not the operator's
  // setting: the same inputs on every PC, or two PCs' results are not comparable.
  const bool bitonal = (m_config.syntheticKind == QLatin1String("bw"));
  const int compression = bitonal ? 4 : 5;

  // Pages left by an earlier run are reused only if they were made the same way.
  const QString stamp = QStringLiteral("v1 kind=%1 dpi=%2 spread=%3 compression=%4")
                            .arg(m_config.syntheticKind)
                            .arg(dpi)
                            .arg(m_config.syntheticSpreadEvery)
                            .arg(compression);
  const QString stampPath = QDir(dir).absoluteFilePath(QStringLiteral(".synthetic"));
  {
    QFile stampFile(stampPath);
    const bool reuse = stampFile.open(QIODevice::ReadOnly) && (QString::fromUtf8(stampFile.readAll()) == stamp);
    stampFile.close();
    if (!reuse) {
      QFile::remove(stampPath);
      for (const QFileInfo& info :
           QDir(dir).entryInfoList({QStringLiteral("page*.tif"), QStringLiteral("page*.part")}, QDir::Files)) {
        QFile::remove(info.absoluteFilePath());
      }
    }
  }

  const auto generateOne = [&](const int i) -> bool {
    const QString path = QDir(dir).absoluteFilePath(QString::asprintf("page%04d.tif", i + 1));
    if (QFileInfo::exists(path)) {
      return true;
    }
    const bool spread = (m_config.syntheticSpreadEvery > 0) && ((i + 1) % m_config.syntheticSpreadEvery == 0);
    const QSize size = spread ? QSize(a4.width() * 2, a4.height()) : a4;
    QRandomGenerator rng(static_cast<quint32>(1000 + i));

    QImage canvas(size, QImage::Format_RGB32);
    canvas.fill(QColor(40, 40, 40));
    {
      QPainter painter(&canvas);
      const int margin = dpi / 5;
      const int pages = spread ? 2 : 1;
      const int pageWidth = (size.width() - 2 * margin) / pages;
      for (int p = 0; p < pages; ++p) {
        const QRect page(margin + p * pageWidth, margin, pageWidth, size.height() - 2 * margin);
        const double skew = (rng.bounded(400) - 200) / 100.0;  // -2..+2 degrees
        painter.save();
        painter.translate(page.center());
        painter.rotate(skew);
        painter.translate(-page.center());
        painter.fillRect(page, QColor(236, 232, 222));
        const QRect text = page.adjusted(dpi * 2 / 3, dpi * 3 / 4, -dpi * 2 / 3, -dpi * 3 / 4);
        for (int y = text.top(); y + dpi / 12 < text.bottom(); y += dpi / 6) {
          int x = text.left();
          while (true) {
            const int w = dpi / 12 + static_cast<int>(rng.bounded(dpi / 2));
            if (x + w >= text.right()) {
              break;
            }
            painter.fillRect(QRect(x, y, w, dpi / 12), QColor(25, 25, 25));
            x += w + dpi / 20;
          }
        }
        // A few specks for despeckling to find.
        for (int s = 0; s < 40; ++s) {
          const QPoint at(page.left() + static_cast<int>(rng.bounded(page.width())),
                          page.top() + static_cast<int>(rng.bounded(page.height())));
          painter.fillRect(QRect(at, QSize(3, 3)), QColor(30, 30, 30));
        }
        painter.restore();
      }
      if (spread) {
        painter.fillRect(QRect(size.width() / 2 - dpi / 30, 0, dpi / 15, size.height()), QColor(60, 60, 60));
      }
    }

    QImage out;
    if (m_config.syntheticKind == QLatin1String("bw")) {
      out = imageproc::BinaryImage(canvas).toQImage();
    } else if (m_config.syntheticKind == QLatin1String("rgb")) {
      out = canvas;
    } else {
      out = imageproc::toGrayscale(canvas);
    }
    // Mandatory: without it the TIFF gets the screen's 96 dpi, and the output
    // stage would upscale every page six-fold.
    out.setDotsPerMeterX(dpm);
    out.setDotsPerMeterY(dpm);
    // Under a temporary name, so a run killed mid-write never leaves a truncated
    // page with the final name for the next run to trip over.
    const QString part = path + QStringLiteral(".part");
    QFile::remove(part);
    return TiffWriter::writeImage(part, out) && QFile::rename(part, path);
  };

  // TiffWriter takes its compression from the settings; set it for the duration.
  ApplicationSettings& settings = ApplicationSettings::getInstance();
  const int savedBw = settings.getTiffBwCompression();
  const int savedColor = settings.getTiffColorCompression();
  settings.setTiffBwCompression(4);
  settings.setTiffColorCompression(5);

  // On a thread of its own. Generating on the GUI thread would itself show up in
  // the log as a stall of the application, and the report would blame it on
  // Scantailor.
  std::atomic<bool> done{false};
  bool ok = true;
  std::thread generator([&]() {
    diag::setThreadRole("other");
    for (int i = 0; i < m_config.syntheticCount && ok; ++i) {
      ok = generateOne(i);
    }
    done.store(true);
  });
  waitFor([&done]() { return done.load(); }, 3600 * 1000LL);
  generator.join();

  settings.setTiffBwCompression(savedBw);
  settings.setTiffColorCompression(savedColor);

  if (ok) {
    QFile stampFile(stampPath);
    if (stampFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      stampFile.write(stamp.toUtf8());
    }
  }

  diag::event("stress.generate",
              {diag::Attr("count", m_config.syntheticCount), diag::Attr("kind", m_config.syntheticKind),
               diag::Attr("dpi", dpi), diag::Attr("compression", compression),
               diag::Attr("dur", timer.nsecsElapsed() / 1e6), diag::Attr("ok", ok)});
  return ok;
}

/*------------------------------ Project handling -----------------------------*/

bool StressDriver::createProject(const QString& outDir) {
  m_step = QStringLiteral("create");
  QElapsedTimer timer;
  timer.start();

  std::vector<ImageFileInfo> files;
  {
    // The New Project dialog reads the same headers on the GUI thread, so this is
    // real application behaviour; the scope names it in any stall it causes.
    DIAG_SCOPE(metadataScope, "stress.create_metadata");
    for (const QString& path : m_scans) {
      std::vector<ImageMetadata> metadata;
      const ImageMetadataLoader::Status status
          = ImageMetadataLoader::load(path, [&](const ImageMetadata& m) { metadata.push_back(m); });
      if (status != ImageMetadataLoader::LOADED || metadata.empty()) {
        failure(QStringLiteral("cannot read image metadata: ") + path);
        continue;
      }
      for (ImageMetadata& m : metadata) {
        // What the Fix DPI dialog would make the operator do.
        if (!m.isDpiOK()) {
          m.setDpi(Dpi(300, 300));
        }
      }
      files.emplace_back(QFileInfo(path), metadata);
    }
  }
  if (files.empty()) {
    return false;
  }
  // switchToNewProject() opens the relinking dialog when the output folder is missing.
  QDir().mkpath(outDir);

  m_loadDone = false;
  m_window->switchToNewProject(std::make_shared<ProjectPages>(files, ProjectPages::AUTO_PAGES, Qt::LeftToRight),
                               outDir);
  const bool loaded = waitForLoad(m_config.pageLoadTimeoutSec * 1000LL);
  if (!m_window) {
    return false;
  }
  diag::event("stress.step",
              {diag::Attr("cycle", m_cycle), diag::Attr("step", "create"),
               diag::Attr("dur", timer.nsecsElapsed() / 1e6), diag::Attr("ok", loaded),
               diag::Attr("images", static_cast<qint64>(files.size())),
               diag::Attr("pages", m_window->allPages().numPages())});
  if (!loaded) {
    failure(QStringLiteral("first page did not load after creating the project"));
  }
  return m_window->isProjectLoaded();
}

bool StressDriver::openProject() {
  m_step = QStringLiteral("open");
  QElapsedTimer timer;
  timer.start();
  m_loadDone = false;
  m_window->openProject(projectFilePath());
  const bool loaded = m_window && m_window->isProjectLoaded() && waitForLoad(m_config.pageLoadTimeoutSec * 1000LL);
  if (!m_window) {
    return false;
  }
  diag::event("stress.step", {diag::Attr("cycle", m_cycle), diag::Attr("step", "open"),
                              diag::Attr("dur", timer.nsecsElapsed() / 1e6), diag::Attr("ok", loaded),
                              diag::Attr("pages", m_window->allPages().numPages())});
  if (!loaded) {
    failure(QStringLiteral("project did not open"));
  }
  return m_window->isProjectLoaded();
}

/** Save As without the file dialog, mirroring MainWindow::saveProjectAsTriggered(). */
bool StressDriver::saveProjectAs(const QString& path) {
  m_step = QStringLiteral("save_as");
  waitIdle(m_config.pageLoadTimeoutSec * 1000LL);
  if (!alive()) {
    return false;
  }
  QElapsedTimer timer;
  timer.start();
  const bool ok = m_window->saveProjectWithFeedback(path);
  if (ok) {
    if (!m_window->m_projectFile.isEmpty() && (m_window->m_projectFile != path)) {
      ProjectRecovery::discardSnapshot(m_window->m_projectFile);
    }
    m_window->m_projectFile = path;
    m_window->updateWindowTitle();
    CrashHandler::setCurrentProjectFile(path);
    m_window->updateAutoSaveTimer();
    m_window->releaseUnsavedSession();

    ProjectHistory history;
    history.read();
    history.touch(path, m_window->m_pages->numImages(), m_window->m_outFileNameGen.outDir());
    history.write();
  }
  diag::event("stress.step", {diag::Attr("cycle", m_cycle), diag::Attr("step", "save_as"),
                              diag::Attr("dur", timer.nsecsElapsed() / 1e6), diag::Attr("ok", ok)});
  if (!ok) {
    failure(QStringLiteral("Save As failed: ") + path);
  }
  return ok;
}

/**
 * Through the same path as File > Close, so the backup write and comparison the
 * operator pays for on every close are measured too. The project was saved just
 * before, so no prompt is expected; one appearing is a failure.
 */
bool StressDriver::closeProject() {
  m_step = QStringLiteral("close");
  waitIdle(m_config.pageLoadTimeoutSec * 1000LL);
  if (!alive()) {
    return false;
  }
  QElapsedTimer timer;
  timer.start();
  const bool closed = m_window->closeProjectInteractive();
  diag::event("stress.step", {diag::Attr("cycle", m_cycle), diag::Attr("step", "close"),
                              diag::Attr("dur", timer.nsecsElapsed() / 1e6), diag::Attr("ok", closed)});
  if (!closed && m_window) {
    failure(QStringLiteral("closing the project did not complete"));
    m_window->releaseUnsavedSession();
    ProjectRecovery::discardSnapshot(m_window->m_projectFile);
    m_window->closeProjectWithoutSaving();
  }
  return closed;
}

/*------------------------------ Stage operations -----------------------------*/

int StressDriver::currentStage() const {
  return m_window ? m_window->m_curFilter : -1;
}

bool StressDriver::switchStage(const int stage) {
  const int from = currentStage();
  if (from == stage) {
    return true;
  }
  m_step = QStringLiteral("switch_stage");
  waitIdle(m_config.pageLoadTimeoutSec * 1000LL);
  if (!alive()) {
    return false;
  }
  QElapsedTimer timer;
  timer.start();
  m_loadDone = false;
  m_window->filterList->selectRow(stage);
  const bool loaded = waitForLoad(m_config.pageLoadTimeoutSec * 1000LL);
  if (!m_window) {
    return false;
  }
  diag::event("stress.stage_switch",
              {diag::Attr("cycle", m_cycle), diag::Attr("from", from), diag::Attr("to", stage),
               diag::Attr("dur", timer.nsecsElapsed() / 1e6), diag::Attr("ok", loaded),
               diag::Attr("outcome", outcomeName(loaded ? m_loadOutcome : -1))});
  if (!loaded) {
    failure(QStringLiteral("switching to stage %1 did not finish loading").arg(stageName(stage)));
  }
  return currentStage() == stage;
}

bool StressDriver::batchStage(const int stage, const bool rebatch) {
  if (!switchStage(stage)) {
    if (alive()) {
      failure(QStringLiteral("could not switch to stage %1 for batch processing").arg(stageName(stage)));
    }
    return false;
  }
  m_step = QStringLiteral("batch");
  waitIdle(m_config.pageLoadTimeoutSec * 1000LL);
  if (!alive()) {
    return false;
  }

  const PageInfo first = m_window->m_thumbSequence->firstPage();
  if (first.isNull()) {
    failure(QStringLiteral("no page to start batch processing from"));
    return false;
  }
  // Batch processing starts from the selected page, not from the first one.
  m_window->m_thumbSequence->setSelection(first.id());
  const int pages = m_window->m_thumbSequence->toPageSequence().numPages();

  QElapsedTimer timer;
  timer.start();
  m_batchDone = false;
  m_batchCompleted = false;
  m_window->startBatchProcessing();
  const qint64 timeoutMs = static_cast<qint64>(pages) * m_config.batchTimeoutSecPerPage * 1000 + 60000;
  bool ok = waitFor([this]() { return m_batchDone; }, timeoutMs);
  const double durationMs = timer.nsecsElapsed() / 1e6;
  if (!m_window) {
    return false;
  }
  if (!ok) {
    failure(QStringLiteral("batch processing at stage %1 timed out").arg(stageName(stage)));
    if (m_window->isBatchProcessingInProgress()) {
      m_window->stopBatchProcessing();
    }
  } else if (!m_batchCompleted) {
    ok = false;
    failure(QStringLiteral("batch processing at stage %1 was interrupted").arg(stageName(stage)));
  }
  // The batch ends by loading a page interactively; let that finish too.
  waitIdle(m_config.pageLoadTimeoutSec * 1000LL);

  // "rebatch": the Output batch of a reopened title, where every page is already
  // done. It measures confirming the cache, not processing, and a report must
  // not average the two.
  diag::event("stress.batch",
              {diag::Attr("cycle", m_cycle), diag::Attr("stage", stage), diag::Attr("stage_name", stageName(stage)),
               diag::Attr("pages", pages), diag::Attr("dur", durationMs), diag::Attr("ok", ok),
               diag::Attr("rebatch", rebatch),
               diag::Attr("sec_per_page", pages > 0 ? durationMs / 1000.0 / pages : 0.0)});
  return ok;
}

bool StressDriver::navigateStage(const int stage) {
  if (!switchStage(stage)) {
    return false;
  }
  m_step = QStringLiteral("navigate");
  waitIdle(m_config.pageLoadTimeoutSec * 1000LL);

  bool allOk = true;
  for (int index = 0; index < m_config.navPages && alive(); ++index) {
    if (index > 0) {
      const PageInfo leader = m_window->m_thumbSequence->selectionLeader();
      if (m_window->m_thumbSequence->nextPage(leader.id()).isNull()) {
        break;
      }
      // Operators look at a page before moving on. The pause also lets the
      // high-quality rendering and the thumbnails catch up, as they would.
      pump(m_config.dwellMs);
      if (!alive()) {
        break;
      }
    }
    QElapsedTimer timer;
    timer.start();
    m_loadDone = false;
    if (index == 0) {
      m_window->goFirstPage();
    } else {
      m_window->goNextPage();
    }
    const bool loaded = waitForLoad(m_config.pageLoadTimeoutSec * 1000LL);
    if (!m_window) {
      return false;
    }
    const int outcome = loaded ? m_loadOutcome : -1;
    diag::event("stress.page_load",
                {diag::Attr("cycle", m_cycle), diag::Attr("stage", stage),
                 diag::Attr("stage_name", stageName(stage)), diag::Attr("index", index),
                 diag::Attr("dur", timer.nsecsElapsed() / 1e6), diag::Attr("outcome", outcomeName(outcome))});
    if (outcome != MainWindow::LOAD_OK) {
      allOk = false;
      failure(QStringLiteral("page %1 at stage %2: %3").arg(index).arg(stageName(stage)).arg(outcomeName(outcome)));
    }
  }
  return allOk;
}

void StressDriver::scrollThumbnails(const int stage) {
  QScrollBar* bar = m_window->thumbView->verticalScrollBar();
  const int maximum = bar->maximum();
  if (maximum <= 0) {
    return;
  }
  m_step = QStringLiteral("thumb_scroll");
  const int steps = 20;
  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i <= steps && alive(); ++i) {
    bar->setValue(maximum * i / steps);
    pump(100);
  }
  if (!alive()) {
    return;
  }
  bar->setValue(0);
  pump(100);
  diag::event("stress.thumb_scroll", {diag::Attr("cycle", m_cycle), diag::Attr("stage", stage),
                                      diag::Attr("steps", steps), diag::Attr("dur", timer.nsecsElapsed() / 1e6)});
}

void StressDriver::runAutosave() {
  m_step = QStringLiteral("autosave");
  for (int i = 0; i < m_config.autosavesPerCycle && alive(); ++i) {
    waitIdle(m_config.pageLoadTimeoutSec * 1000LL);
    if (!alive()) {
      break;
    }
    QElapsedTimer timer;
    timer.start();
    const bool wrote = m_window->autoSaveProject();
    diag::event("stress.autosave", {diag::Attr("cycle", m_cycle), diag::Attr("dur", timer.nsecsElapsed() / 1e6),
                                    diag::Attr("ok", wrote), diag::Attr("branch", m_window->m_lastAutoSaveBranch)});
    pump(m_config.dwellMs);
  }
}

/**
 * Waits for the previous project's objects to actually be released, then lets
 * the process settle before sampling. Sampling any earlier would count memory
 * that is merely waiting to be freed as a leak.
 */
void StressDriver::settle() {
  m_step = QStringLiteral("settle");
  // The whole scenario runs inside one call from the event loop, and Qt runs a
  // deleteLater() only once control is back in the loop that was running when it
  // was called - which for this driver would be the end of the run. In real use
  // control returns there after every click. Without this, what the project
  // opening context and the old stage-list model hold on to would pile up cycle
  // after cycle and show up as a leak that operators never have.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  if (!alive()) {
    return;
  }
  waitFor(
      [this]() {
        return !m_window
               || (m_window->m_retiredProjectObjects.empty() && m_window->m_workerThreadPool->isIdle());
      },
      60000);
  pump(m_config.settleMs);
  diag::sampleNow("cycle_end", {diag::Attr("cycle", m_cycle)});
}

/*---------------------------------- Waiting ----------------------------------*/

bool StressDriver::isIdle() const {
  return m_window && !m_window->isBatchProcessingInProgress() && m_window->m_interactiveQueue->allProcessed()
         && m_window->m_workerThreadPool->isIdle();
}

/**
 * Runs the event loop until \p condition holds. Everything the application does
 * - task results, timers, painting, the heartbeat - keeps happening meanwhile,
 * exactly as it would while an operator waits.
 */
bool StressDriver::waitFor(const std::function<bool()>& condition, const qint64 timeoutMs) {
  if (condition()) {
    return true;
  }
  QElapsedTimer timer;
  timer.start();
  QEventLoop loop;
  QTimer tick;
  tick.setInterval(10);
  connect(&tick, &QTimer::timeout, &loop, [&]() {
    if (condition() || m_fatal || !m_window || timer.elapsed() > timeoutMs) {
      loop.quit();
    }
  });
  tick.start();
  ++m_waitDepth;
  loop.exec();
  --m_waitDepth;
  return m_window && condition();
}

bool StressDriver::waitIdle(const qint64 timeoutMs) {
  return waitFor([this]() { return isIdle(); }, timeoutMs);
}

bool StressDriver::waitForLoad(const qint64 timeoutMs) {
  return waitFor([this]() { return m_loadDone; }, timeoutMs);
}

void StressDriver::pump(const int ms) {
  if (ms <= 0) {
    return;
  }
  QElapsedTimer timer;
  timer.start();
  waitFor([&timer, ms]() { return timer.elapsed() >= ms; }, ms + 1000);
}

void StressDriver::onInteractiveLoadFinished(const PageId&, int, const int outcome) {
  m_loadOutcome = outcome;
  m_loadDone = true;
}

void StressDriver::onBatchFinished(const bool completed) {
  m_batchCompleted = completed;
  m_batchDone = true;
}

/*---------------------------------- Failures ---------------------------------*/

void StressDriver::checkForDialogs() {
  QWidget* dialog = QApplication::activeModalWidget();
  if (!dialog) {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
      if (widget->isVisible() && qobject_cast<QDialog*>(widget)) {
        dialog = widget;
        break;
      }
    }
  }
  if (!dialog) {
    return;
  }

  QString text;
  QString action;
  if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
    text = box->text();
    if (QAbstractButton* escape = box->escapeButton()) {
      action = QStringLiteral("escape:") + escape->text();
      escape->click();
    } else {
      action = QStringLiteral("reject");
      box->reject();
    }
  } else if (auto* plain = qobject_cast<QDialog*>(dialog)) {
    action = QStringLiteral("reject");
    plain->reject();
  } else {
    action = QStringLiteral("close");
    dialog->close();
  }
  diag::event("stress.unexpected_dialog",
              {diag::Attr("class", QString::fromLatin1(dialog->metaObject()->className())),
               diag::Attr("title", dialog->windowTitle()), diag::Attr("text", text), diag::Attr("action", action),
               diag::Attr("cycle", m_cycle), diag::Attr("step", m_step)});
  failure(QStringLiteral("unexpected dialog: ") + dialog->windowTitle());
}

void StressDriver::onOutOfMemory() {
  fatal(QStringLiteral("out of memory"));
}

void StressDriver::failure(const QString& what) {
  ++m_failures;
  diag::event("stress.failure",
              {diag::Attr("what", what), diag::Attr("cycle", m_cycle), diag::Attr("step", m_step)});
  CrashHandler::log(QStringLiteral("Stress failure (cycle %1, %2): %3").arg(m_cycle).arg(m_step, what));
}

void StressDriver::fatal(const QString& what) {
  failure(what);
  m_fatal = true;
}
