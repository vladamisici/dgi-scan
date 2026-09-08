// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "CrashHandler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
// dbghelp.h must follow windows.h.
#include <dbghelp.h>
#endif

namespace core {
namespace {
// Guards the log file. A crash handler may only ever tryLock() it: see
// writeToLogNonBlocking().
QMutex g_logMutex;
QString g_reportDir;
QString g_logPath;
bool g_installed = false;
QtMessageHandler g_previousMessageHandler = nullptr;

// Roll the log over at this size so it cannot grow without bound on a machine
// that is never restarted.
const qint64 MAX_LOG_BYTES = 4 * 1024 * 1024;

#ifdef Q_OS_WIN
// Everything a crash handler touches is pre-computed at install() time: once the
// process is dying we cannot rely on the heap, on Qt, or on the CRT locale.
wchar_t g_reportDirW[MAX_PATH + 2] = {0};  // with trailing backslash
wchar_t g_projectFileW[512] = {0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

const size_t PATH_BUF_CHARS = MAX_PATH + 64;

void appendCrashNote(const wchar_t* stemPath, const char* reason, const void* address, unsigned long code) {
  wchar_t notePath[PATH_BUF_CHARS];
  _snwprintf(notePath, PATH_BUF_CHARS - 1, L"%s.txt", stemPath);
  notePath[PATH_BUF_CHARS - 1] = 0;

  const HANDLE file
      = ::CreateFileW(notePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  SYSTEMTIME st;
  ::GetLocalTime(&st);

  // Formatted wide and converted once, rather than formatted narrow with a %ls
  // for the project path. The narrow printf converts %ls through the CRT's ANSI
  // codepage and returns -1 for the whole call the moment one character will not
  // fit - so a project path containing, say, a Romanian s-comma or any Cyrillic
  // or CJK character discarded the entire report, including the plain-ASCII
  // reason and fault address, leaving a zero-byte file.
  wchar_t noteW[2048];
  const int wlen = _snwprintf(noteW, (sizeof(noteW) / sizeof(noteW[0])) - 1,
                              L"ScanTailor Advanced crash report\r\n"
                              L"time      : %04u-%02u-%02u %02u:%02u:%02u\r\n"
                              L"reason    : %S\r\n"
                              L"code      : 0x%08lx\r\n"
                              L"address   : %p\r\n"
                              L"process   : %lu (%u-bit)\r\n"
                              L"thread    : %lu\r\n"
                              L"project   : %s\r\n",
                              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, reason, code, address,
                              ::GetCurrentProcessId(), static_cast<unsigned>(sizeof(void*) * 8),
                              ::GetCurrentThreadId(), g_projectFileW[0] ? g_projectFileW : L"<none>");
  // A negative return means truncation, not "produce nothing": write whatever
  // was formatted rather than throwing the report away.
  noteW[(sizeof(noteW) / sizeof(noteW[0])) - 1] = 0;
  const int wcount = (wlen > 0) ? wlen : static_cast<int>(::wcslen(noteW));

  if (wcount > 0) {
    char utf8[4096];
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, noteW, wcount, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes > 0) {
      DWORD written = 0;
      ::WriteFile(file, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    }
  }
  ::CloseHandle(file);
}

/**
 * Writes a minidump plus a human-readable note. \p exceptionInfo may be null,
 * in which case the stacks of all threads are still captured, which is what we
 * need for a std::terminate() or an abort().
 */
void writeCrashReport(EXCEPTION_POINTERS* exceptionInfo, const char* reason) {
  if (!g_reportDirW[0]) {
    return;
  }

  // Only the first handler to run reports. These handlers chain - the terminate
  // handler ends in abort(), which raises SIGABRT - and a second dump of a
  // process that is already dying adds nothing but confusion for whoever reads
  // the crash folder.
  static std::atomic_flag reported = ATOMIC_FLAG_INIT;
  if (reported.test_and_set()) {
    return;
  }

  SYSTEMTIME st;
  ::GetLocalTime(&st);

  wchar_t stem[PATH_BUF_CHARS];
  _snwprintf(stem, PATH_BUF_CHARS - 1, L"%sscantailor-%04u%02u%02u-%02u%02u%02u-%lu", g_reportDirW, st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ::GetCurrentProcessId());
  stem[PATH_BUF_CHARS - 1] = 0;

  const void* address = nullptr;
  unsigned long code = 0;
  if (exceptionInfo && exceptionInfo->ExceptionRecord) {
    address = exceptionInfo->ExceptionRecord->ExceptionAddress;
    code = exceptionInfo->ExceptionRecord->ExceptionCode;
  }
  appendCrashNote(stem, reason, address, code);

  wchar_t dumpPath[PATH_BUF_CHARS];
  _snwprintf(dumpPath, PATH_BUF_CHARS - 1, L"%s.dmp", stem);
  dumpPath[PATH_BUF_CHARS - 1] = 0;

  const HANDLE file
      = ::CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  MINIDUMP_EXCEPTION_INFORMATION mei;
  mei.ThreadId = ::GetCurrentThreadId();
  mei.ExceptionPointers = exceptionInfo;
  mei.ClientPointers = FALSE;

  // Deliberately not MiniDumpWithFullMemory: this process can legitimately hold
  // a couple of gigabytes of page images, and a dump that large would never make
  // it back to us from an operator's machine. The flags below keep dumps in the
  // low tens of megabytes while still giving usable stacks and the objects those
  // stacks point at.
  const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
      MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo
      | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);

  ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, dumpType, exceptionInfo ? &mei : nullptr,
                      nullptr, nullptr);
  ::CloseHandle(file);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
  writeCrashReport(exceptionInfo, "unhandled structured exception");
  if (g_previousFilter) {
    return g_previousFilter(exceptionInfo);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

#ifdef _MSC_VER
void invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  writeCrashReport(nullptr, "invalid CRT parameter");
  ::TerminateProcess(::GetCurrentProcess(), 0xC0000417);
}

void pureCallHandler() {
  writeCrashReport(nullptr, "pure virtual function call");
  ::TerminateProcess(::GetCurrentProcess(), 0xC0000025);
}
#endif  // _MSC_VER
#endif  // Q_OS_WIN

/** Appends one line. The caller must hold g_logMutex. */
void appendLineLocked(const QString& line) {
  if (g_logPath.isEmpty()) {
    return;
  }
  QFile file(g_logPath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    return;
  }
  QTextStream stream(&file);
  stream.setCodec("UTF-8");
  stream << line << '\n';
  stream.flush();
  file.close();

  // Checked here rather than only at startup: a machine that is never restarted
  // would otherwise let the log grow without bound despite MAX_LOG_BYTES.
  if (file.size() >= MAX_LOG_BYTES) {
    const QString previous = g_logPath + QLatin1String(".1");
    QFile::remove(previous);
    QFile::rename(g_logPath, previous);
  }
}

void writeToLog(const QString& line) {
  const QMutexLocker locker(&g_logMutex);
  appendLineLocked(line);
}

/**
 * \brief Logs from a crash handler, giving up rather than waiting for the lock.
 *
 * The thread that is dying may be the one already holding g_logMutex - it can be
 * killed by an allocation failure inside appendLineLocked() itself. Blocking
 * there would hang the process instead of letting it write its dump and exit,
 * which is precisely the "the window just froze" behaviour this work exists to
 * remove.
 */
void writeToLogNonBlocking(const QString& line) {
  if (!g_logMutex.tryLock()) {
    return;
  }
  appendLineLocked(line);
  g_logMutex.unlock();
}

/**
 * The handler that actually matters for the "it closes with no message" reports:
 * when a background task lets an exception escape QRunnable::run(), the C++
 * runtime calls std::terminate(), which kills the process without any UI.
 */
void terminateHandler() {
  const char* reason = "std::terminate (uncaught exception)";
  static char detail[512];

  if (std::exception_ptr current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::bad_alloc&) {
      reason = "std::terminate (out of memory - uncaught std::bad_alloc)";
    } catch (const std::exception& e) {
      std::snprintf(detail, sizeof(detail), "std::terminate (uncaught std::exception: %s)", e.what());
      reason = detail;
    } catch (...) {
      reason = "std::terminate (uncaught non-standard exception)";
    }
  }

#ifdef Q_OS_WIN
  writeCrashReport(nullptr, reason);
#endif
  writeToLogNonBlocking(QString::fromLatin1("%1 [FATAL] %2")
                            .arg(QDateTime::currentDateTime().toString(Qt::ISODate), QString::fromUtf8(reason)));

  // The contract of a terminate handler is that it never returns.
  std::abort();
}

extern "C" void signalHandler(int signalNumber) {
  const char* reason = "fatal signal";
  switch (signalNumber) {
    case SIGABRT:
      reason = "abort()";
      break;
    case SIGSEGV:
      reason = "segmentation fault";
      break;
    case SIGFPE:
      reason = "arithmetic exception";
      break;
    case SIGILL:
      reason = "illegal instruction";
      break;
    default:
      break;
  }

#ifdef Q_OS_WIN
  writeCrashReport(nullptr, reason);
#else
  writeToLogNonBlocking(QString::fromLatin1("FATAL: ") + QString::fromUtf8(reason));
#endif

  ::signal(signalNumber, SIG_DFL);
  ::raise(signalNumber);
}

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  const char* level = "INFO";
  switch (type) {
    case QtDebugMsg:
      level = "DEBUG";
      break;
    case QtInfoMsg:
      level = "INFO";
      break;
    case QtWarningMsg:
      level = "WARN";
      break;
    case QtCriticalMsg:
      level = "ERROR";
      break;
    case QtFatalMsg:
      level = "FATAL";
      break;
  }

  writeToLog(QString::fromLatin1("%1 [%2] %3")
                 .arg(QDateTime::currentDateTime().toString(Qt::ISODate), QString::fromLatin1(level), message));

  if (g_previousMessageHandler) {
    g_previousMessageHandler(type, context, message);
  }
}

/** Renames the log aside once it gets too big, keeping exactly one old copy. */
void rotateLogIfNeeded(const QString& path) {
  const QFileInfo info(path);
  if (!info.exists() || (info.size() < MAX_LOG_BYTES)) {
    return;
  }
  const QString previous = path + QLatin1String(".1");
  QFile::remove(previous);
  QFile::rename(path, previous);
}

/** Returns \p candidate if we can actually create and write files there. */
QString ensureWritableDir(const QString& candidate) {
  if (candidate.isEmpty()) {
    return QString();
  }
  QDir dir(candidate);
  if (!dir.exists() && !dir.mkpath(QLatin1String("."))) {
    return QString();
  }
  QFile probe(dir.absoluteFilePath(QLatin1String(".write-probe")));
  if (!probe.open(QIODevice::WriteOnly)) {
    return QString();
  }
  probe.close();
  probe.remove();
  return dir.absolutePath();
}
}  // namespace

void CrashHandler::install(const QString& reportDir) {
  if (g_installed) {
    return;
  }

  QString resolved = ensureWritableDir(reportDir);
  if (resolved.isEmpty()) {
    // Falling back keeps diagnostics working for a portable install on a
    // read-only medium, or when the configured location is on a share that is
    // currently unreachable - exactly the situation we want reports from.
    const QString fallback
        = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QLatin1String("/scantailor-crashes");
    resolved = ensureWritableDir(fallback);
  }
  if (resolved.isEmpty()) {
    return;
  }

  g_reportDir = resolved;
  const QString logPath = QDir(resolved).absoluteFilePath(QLatin1String("scantailor.log"));
  rotateLogIfNeeded(logPath);
  {
    const QMutexLocker locker(&g_logMutex);
    g_logPath = logPath;
  }

#ifdef Q_OS_WIN
  {
    QString nativeDir = QDir::toNativeSeparators(resolved);
    if (!nativeDir.endsWith(QLatin1Char('\\'))) {
      nativeDir += QLatin1Char('\\');
    }
    const int copied = nativeDir.left(MAX_PATH).toWCharArray(g_reportDirW);
    g_reportDirW[copied] = 0;
  }
  g_previousFilter = ::SetUnhandledExceptionFilter(&unhandledExceptionFilter);
#ifdef _MSC_VER
  _set_invalid_parameter_handler(&invalidParameterHandler);
  _set_purecall_handler(&pureCallHandler);
#endif
#endif  // Q_OS_WIN

  std::set_terminate(&terminateHandler);

  ::signal(SIGABRT, &signalHandler);
  ::signal(SIGSEGV, &signalHandler);
  ::signal(SIGFPE, &signalHandler);
  ::signal(SIGILL, &signalHandler);

  g_previousMessageHandler = qInstallMessageHandler(&messageHandler);
  g_installed = true;

  log(QString::fromLatin1("--- ScanTailor Advanced started (pid %1, %2-bit) ---")
          .arg(QCoreApplication::applicationPid())
          .arg(sizeof(void*) * 8));
}

QString CrashHandler::reportDir() {
  return g_reportDir;
}

QString CrashHandler::logFilePath() {
  const QMutexLocker locker(&g_logMutex);
  return g_logPath;
}

void CrashHandler::setCurrentProjectFile(const QString& projectFilePath) {
#ifdef Q_OS_WIN
  const int maxChars = static_cast<int>(sizeof(g_projectFileW) / sizeof(g_projectFileW[0])) - 1;
  const int copied = projectFilePath.left(maxChars).toWCharArray(g_projectFileW);
  g_projectFileW[copied] = 0;
#endif
  if (!projectFilePath.isEmpty()) {
    log(QString::fromLatin1("Project opened: ") + projectFilePath);
  }
}

void CrashHandler::log(const QString& message) {
  writeToLog(QString::fromLatin1("%1 [INFO] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), message));
}
}  // namespace core
