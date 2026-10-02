// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "AppUpdater.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThread>
#include <algorithm>
#include <memory>

#include "Application.h"
#include "CrashHandler.h"
#include "Diagnostics.h"
#include "Utils.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

using core::AppUpdate;
using core::CrashHandler;
using core::ReleaseVersion;

// The messages are in Romanian without diacritics, as the operators asked, and
// so are not marked for translation.
namespace {
// A folder: every scantailor-dgi-*-win64.exe in it counts.
const char DEFAULT_LOCATION[] = "\\\\server1\\kit\\PoD_and_XML";
const int FIRST_CHECK_MS = 3000;
const int CHECK_INTERVAL_MS = 60 * 60 * 1000;
const int DEFAULT_COUNTDOWN_SECONDS = 20;

const char ENABLED_KEY[] = "update/enabled";
const char LOCATION_KEY[] = "update/location";
// The version an update was started for, kept until the next start tells whether it took.
const char ATTEMPTED_KEY[] = "update/attemptedVersion";
const char ATTEMPTED_FROM_KEY[] = "update/attemptedFrom";
// A version that failed to install is not offered again unasked.
const char FAILED_KEY[] = "update/failedVersion";
// How long an unanswered offer waits before going ahead; 0 waits for an answer.
const char COUNTDOWN_KEY[] = "update/countdownSeconds";

int countdownSeconds() {
  return std::max(0, QSettings().value(QLatin1String(COUNTDOWN_KEY), DEFAULT_COUNTDOWN_SECONDS).toInt());
}

QString title() {
  return QStringLiteral("Actualizare");
}

QString updateDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
      .filePath(QStringLiteral("scantailor-dgi-update"));
}

/** \brief A PowerShell string literal: single quotes, with any inside doubled. */
QString psQuote(const QString& text) {
  QString quoted(text);
  quoted.replace(QLatin1Char('\''), QLatin1String("''"));
  return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

/**
 * \brief Whether another running copy uses this installation.
 *
 * A copy whose path cannot be read - another user's, on a shared machine -
 * counts: the installer would remove what it could of the installation and
 * then stop at the files that copy holds.
 */
bool installationInUseElsewhere() {
#ifdef Q_OS_WIN
  const QString ownPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
  const QString ownName = QFileInfo(ownPath).fileName();
  const DWORD ownPid = ::GetCurrentProcessId();
  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool inUse = false;
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !inUse; more = ::Process32NextW(snapshot, &entry)) {
    if ((entry.th32ProcessID == ownPid)
        || (QString::fromWCharArray(entry.szExeFile).compare(ownName, Qt::CaseInsensitive) != 0)) {
      continue;
    }
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (!process) {
      inUse = true;
      break;
    }
    wchar_t path[1024];
    DWORD size = DWORD(sizeof(path) / sizeof(path[0]));
    // A copy run from elsewhere - a build being tested - holds nothing of this installation.
    if (!::QueryFullProcessImageNameW(process, 0, path, &size)
        || (QString::fromWCharArray(path, int(size)).compare(ownPath, Qt::CaseInsensitive) == 0)) {
      inUse = true;
    }
    ::CloseHandle(process);
  }
  ::CloseHandle(snapshot);
  return inUse;
#else
  return false;
#endif
}
}  // namespace

AppUpdater::AppUpdater(QMainWindow* window, std::function<bool()> busy)
    : QObject(window), m_window(window), m_busy(std::move(busy)) {
  m_timer.setSingleShot(true);
  connect(&m_timer, &QTimer::timeout, this, &AppUpdater::check);
}

// A check stuck on an unreachable share is not waited for: its thread deletes
// itself when it is done, or ends with the process.
AppUpdater::~AppUpdater() = default;

bool AppUpdater::isEnabled() {
  const auto* app = qobject_cast<const Application*>(QCoreApplication::instance());
  const bool installed = app && !app->isPortableVersion();
  return QSettings().value(QLatin1String(ENABLED_KEY), installed).toBool();
}

void AppUpdater::start() {
  reportPreviousAttempt();
  m_timer.start(FIRST_CHECK_MS);
}

void AppUpdater::check() {
  m_timer.start(CHECK_INTERVAL_MS);
  if (m_checkThread || m_closing) {
    return;
  }
  const QString location
      = QSettings().value(QLatin1String(LOCATION_KEY), QString::fromLatin1(DEFAULT_LOCATION)).toString();
  auto result = std::make_shared<AppUpdate::Installer>();
  QThread* thread = QThread::create([location, result]() {
    core::diag::setThreadRole("update");
    *result = AppUpdate::newestInstaller(location);
  });
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  connect(thread, &QThread::finished, this, [this, result]() { checkFinished(*result); });
  m_checkThread = thread;
  thread->start(QThread::LowPriority);
}

void AppUpdater::checkFinished(const AppUpdate::Installer& installer) {
  const ReleaseVersion current = AppUpdate::current();
  // Nothing newer. One found before stays on offer: the share may just be out of reach for now.
  if (installer.isNull() || !(current < installer.version)) {
    return;
  }
  const bool firstSeen = m_available.isNull() || (m_available.version != installer.version);
  m_available = installer;
  showNotice();
  if (!firstSeen) {
    return;
  }

  CrashHandler::log(QStringLiteral("Update: version %1 is available at %2 (this is %3)")
                        .arg(installer.version.toString(), QDir::toNativeSeparators(installer.path),
                             current.toString()));
  m_declined = false;
  if (!m_busy() && !failedBefore()) {
    offer(true);
  }
}

bool AppUpdater::failedBefore() const {
  return QSettings().value(QLatin1String(FAILED_KEY)).toString() == m_available.version.toString();
}

void AppUpdater::becameIdle() {
  if (m_available.isNull() || m_declined || m_closing || failedBefore()) {
    return;
  }
  // Once the start page is up, rather than from inside the code putting it up.
  QTimer::singleShot(0, this, [this]() {
    if (!m_busy() && !m_closing) {
      offer(true);
    }
  });
}

void AppUpdater::offer(const bool countdown) {
  if (m_asking || m_closing || m_available.isNull()) {
    return;
  }
  // A dialog of the operator's is open: the question would land on top of it.
  if (QApplication::activeModalWidget()) {
    QTimer::singleShot(30000, this, [this, countdown]() {
      if (!m_busy()) {
        offer(countdown);
      }
    });
    return;
  }

  const QString text = QStringLiteral("Versiune noua disponibila: %1\n\n"
                                      "Aplicatia se inchide, se actualizeaza si porneste din nou.")
                           .arg(m_available.version.toString());
  if (!ask(text, QStringLiteral("Mai tarziu"), countdown ? countdownSeconds() : 0)) {
    m_declined = true;
    CrashHandler::log(QStringLiteral("Update: %1 put off").arg(m_available.version.toString()));
    return;
  }
  requestInstall();
}

bool AppUpdater::ask(const QString& text, const QString& declineText, const int countdown) {
  m_asking = true;
  QMessageBox box(QMessageBox::Information, title(), text, QMessageBox::NoButton, m_window);
  QPushButton* const update = box.addButton(QStringLiteral("Actualizeaza"), QMessageBox::AcceptRole);
  QPushButton* const decline = box.addButton(declineText, QMessageBox::RejectRole);
  box.setDefaultButton(update);
  box.setEscapeButton(decline);

  int remaining = countdown;
  QTimer tick;
  const auto showRemaining = [&]() { update->setText(QStringLiteral("Actualizeaza (%1)").arg(remaining)); };
  if (countdown > 0) {
    showRemaining();
    connect(&tick, &QTimer::timeout, &box, [&]() {
      if (--remaining > 0) {
        showRemaining();
      } else {
        tick.stop();
        update->click();
      }
    });
    tick.start(1000);
  }
  box.exec();
  m_asking = false;
  return box.clickedButton() == update;
}

void AppUpdater::requestInstall() {
  if (installationInUseElsewhere()) {
    QMessageBox::information(m_window, title(),
                             QStringLiteral("Scantailor-DGI este deschis si in alta fereastra.\n"
                                            "Inchide-o, apoi actualizeaza din nou."));
    return;
  }
  QString error;
  if (!prepareInstaller(&error)) {
    CrashHandler::log(QStringLiteral("Update: the installer could not be copied: ") + error);
    QMessageBox::warning(m_window, title(), QStringLiteral("Installerul nu a putut fi copiat.\n\n") + error);
    return;
  }
  m_installRequested = true;
  // Closing asks to save an open project, and can be cancelled there:
  // applicationClosing() or closeCancelled() follows.
  m_window->close();
}

void AppUpdater::closeCancelled() {
  m_installRequested = false;
}

void AppUpdater::applicationClosing() {
  m_closing = true;
  m_timer.stop();
  if (!m_installRequested) {
    // An update found while the operator worked goes in as they leave, unless they say otherwise.
    if (m_available.isNull() || failedBefore() || qApp->isSavingSession() || installationInUseElsewhere()) {
      return;
    }
    const QString text = QStringLiteral("Versiune noua disponibila: %1\n\nSe instaleaza acum, dupa inchidere.")
                             .arg(m_available.version.toString());
    if (!ask(text, QStringLiteral("Nu acum"), countdownSeconds())) {
      return;
    }
    QString error;
    if (!prepareInstaller(&error)) {
      CrashHandler::log(QStringLiteral("Update: the installer could not be copied: ") + error);
      return;
    }
    if (!launchInstaller(false)) {
      QMessageBox::warning(m_window, title(), QStringLiteral("Actualizarea nu a putut porni."));
    }
    return;
  }
  m_installRequested = false;
  if (!launchInstaller(true)) {
    QMessageBox::warning(m_window, title(),
                         QStringLiteral("Actualizarea nu a putut porni. Porneste aplicatia din nou."));
  }
}

bool AppUpdater::prepareInstaller(QString* error) {
  const QString dir = updateDir();
  if (!QDir().mkpath(dir)) {
    *error = QStringLiteral("Folderul %1 nu a putut fi creat.").arg(QDir::toNativeSeparators(dir));
    return false;
  }
  const QString source = m_available.path;
  const ReleaseVersion expected = m_available.version;
  const QString target
      = QDir(dir).filePath(QStringLiteral("scantailor-dgi-%1-win64.exe").arg(expected.toString()));

  bool ok = false;
  QString copyError;
  QThread* thread = QThread::create([&]() {
    core::diag::setThreadRole("update");
    QFile::remove(target);
    QFile installer(source);
    if (!installer.copy(target)) {
      copyError = QStringLiteral("%1\n%2").arg(QDir::toNativeSeparators(source), installer.errorString());
      return;
    }
    // Being replaced on the share while it was copied, say.
    if (AppUpdate::productVersion(target) != expected) {
      copyError = QStringLiteral("Copia nu are versiunea %1.").arg(expected.toString());
      QFile::remove(target);
      return;
    }
    ok = true;
  });
  QEventLoop loop;
  connect(thread, &QThread::finished, &loop, &QEventLoop::quit);

  QProgressDialog progress(QStringLiteral("Se pregateste actualizarea..."), QString(), 0, 0, m_window);
  progress.setWindowTitle(title());
  progress.setWindowModality(Qt::WindowModal);
  progress.setCancelButton(nullptr);
  progress.setMinimumDuration(300);
  progress.setValue(0);

  QApplication::setOverrideCursor(Qt::WaitCursor);
  thread->start();
  loop.exec(QEventLoop::ExcludeUserInputEvents);
  thread->wait();
  delete thread;
  QApplication::restoreOverrideCursor();

  if (!ok) {
    *error = copyError;
    return false;
  }
  m_localInstaller = target;
  return true;
}

bool AppUpdater::launchInstaller(const bool restart) {
  const QString app = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
  const QString installDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
  const qint64 pid = QCoreApplication::applicationPid();

  QStringList script;
  script << QStringLiteral("$ErrorActionPreference = 'SilentlyContinue'")
         << QStringLiteral("$app = %1").arg(psQuote(app))
         // This copy, closing.
         << QStringLiteral("Wait-Process -Id %1 -Timeout 300").arg(pid)
         << QStringLiteral("if (Get-Process -Id %1) { exit 1 }").arg(pid)
         // Anything else running from the installation would leave it half removed.
         << QStringLiteral("if (@(Get-Process -Name %1 | Where-Object { -not $_.Path -or $_.Path -eq $app }).Count -gt 0)"
                           " { exit 2 }")
                .arg(psQuote(QFileInfo(app).completeBaseName()))
         // Silent, into the folder this copy runs from. /D= has to come last, unquoted.
         << QStringLiteral("try { Start-Process -FilePath %1 -ArgumentList %2 -Verb RunAs -Wait } catch { }")
                .arg(psQuote(m_localInstaller), psQuote(QStringLiteral("/S /D=") + installDir));
  if (restart) {
    script << QStringLiteral("Start-Process -FilePath $app");
  }
  const QString command = script.join(QLatin1Char('\n'));
  // Encoded, so that no path needs quoting on the command line.
  const QByteArray encoded
      = QByteArray(reinterpret_cast<const char*>(command.utf16()), command.size() * 2).toBase64();

  QString powershell = QDir(qEnvironmentVariable("SystemRoot"))
                           .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
  if (!QFileInfo::exists(powershell)) {
    powershell = QStringLiteral("powershell.exe");
  }
  QProcess process;
  process.setProgram(powershell);
  process.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"),
                        QStringLiteral("Hidden"), QStringLiteral("-EncodedCommand"), QString::fromLatin1(encoded)});
  process.setWorkingDirectory(updateDir());
  qint64 scriptPid = 0;
  if (!process.startDetached(&scriptPid)) {
    CrashHandler::log(QStringLiteral("Update: the installer could not be started: ") + process.errorString());
    return false;
  }

  QSettings settings;
  settings.setValue(QLatin1String(ATTEMPTED_KEY), m_available.version.toString());
  settings.setValue(QLatin1String(ATTEMPTED_FROM_KEY), m_available.path);
  settings.sync();
  CrashHandler::log(QStringLiteral("Update: installing %1 from %2 once this copy has closed (script pid %3)%4")
                        .arg(m_available.version.toString(), QDir::toNativeSeparators(m_localInstaller))
                        .arg(scriptPid)
                        .arg(restart ? QStringLiteral(", then starting again") : QString()));
  return true;
}

void AppUpdater::reportPreviousAttempt() {
  QSettings settings;
  const QString attempted = settings.value(QLatin1String(ATTEMPTED_KEY)).toString();
  if (attempted.isEmpty()) {
    // Nothing in flight: copies of installers that have run can go.
    QDir(updateDir()).removeRecursively();
    return;
  }
  const QString from = settings.value(QLatin1String(ATTEMPTED_FROM_KEY)).toString();
  settings.remove(QLatin1String(ATTEMPTED_KEY));
  settings.remove(QLatin1String(ATTEMPTED_FROM_KEY));

  const ReleaseVersion current = AppUpdate::current();
  if (!(current < ReleaseVersion::fromString(attempted))) {
    CrashHandler::log(QStringLiteral("Update: now running %1").arg(current.toString()));
    settings.remove(QLatin1String(FAILED_KEY));
    QDir(updateDir()).removeRecursively();
    m_window->statusBar()->showMessage(QStringLiteral("Actualizat la versiunea %1").arg(current.toString()), 20000);
    return;
  }

  // Not offered again unasked: whatever stopped it - the administrator prompt
  // declined, another copy open - would most likely stop it again.
  CrashHandler::log(QStringLiteral("Update: %1 did not install; still running %2").arg(attempted, current.toString()));
  settings.setValue(QLatin1String(FAILED_KEY), attempted);
  QTimer::singleShot(1500, this, [this, attempted, from]() {
    QMessageBox::warning(m_window, title(),
                         QStringLiteral("Versiunea %1 nu s-a instalat.\n\nSe poate instala manual cu:\n%2")
                             .arg(attempted, QDir::toNativeSeparators(from)));
  });
}

void AppUpdater::showNotice() {
  if (!m_notice) {
    m_notice = new QLabel(m_window->statusBar());
    m_notice->setTextFormat(Qt::RichText);
    connect(m_notice.data(), &QLabel::linkActivated, this, [this](const QString&) { offer(false); });
    m_window->statusBar()->insertPermanentWidget(0, m_notice);
  }
  m_notice->setText(QStringLiteral("<b>%1</b>").arg(core::Utils::richTextForLink(QStringLiteral("Versiune noua disponibila"))));
  m_notice->setToolTip(QStringLiteral("Versiunea %1\n%2\n\nClic pentru actualizare.")
                           .arg(m_available.version.toString(), QDir::toNativeSeparators(m_available.path)));
  m_notice->show();
}
