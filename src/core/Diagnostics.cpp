// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "Diagnostics.h"

#include <config.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

#include "version.h"

#ifdef Q_OS_WIN
#include <windows.h>
// windows.h must come first.
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace core {
namespace diag {
namespace {
constexpr int MAX_GUI_OPS = 32;
constexpr qint64 BEAT_INTERVAL_MS = 50;
constexpr qint64 MAX_FILE_BYTES = 64LL * 1024 * 1024;
constexpr size_t MAX_QUEUED_RECORDS = 200000;
constexpr int MAX_STACK_FRAMES = 48;
constexpr int MAX_STACK_CAPTURES = 200;
constexpr size_t STACK_COPY_BYTES = 512 * 1024;

std::atomic<int> g_level{static_cast<int>(Level::Off)};
qint64 g_startNs = 0;
Config g_config;

thread_local const char* t_role = nullptr;
thread_local quint64 t_task = 0;
thread_local int t_depth = 0;
thread_local bool t_isGui = false;

std::atomic<quint64> g_nextTask{1};

// The names of the scopes open on the GUI thread, published for the watchdog.
// Only string literals are ever stored, so reading a stale entry is harmless.
std::atomic<const char*> g_guiOps[MAX_GUI_OPS];
std::atomic<int> g_guiDepth{0};

std::atomic<Counter*> g_counters{nullptr};

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::vector<QByteArray> g_queue;
std::atomic<qint64> g_dropped{0};
std::atomic<qint64> g_records{0};

std::atomic<qint64> g_lastBeatNs{0};
// Written by the beacon: the last gap between two beats longer than two intervals.
std::atomic<qint64> g_gapStartNs{0};
std::atomic<qint64> g_gapEndNs{0};
// Written by the watchdog: the end of the last freeze of the whole process.
std::atomic<qint64> g_pauseEndNs{0};
// Set as soon as a crash report starts: dbghelp is single-threaded, and the
// crash handler's dump must not race the watchdog's symbol lookups.
std::atomic<bool> g_crashing{false};
std::atomic<qint64> g_beatCount{0};
std::atomic<qint64> g_beatLate50{0};
std::atomic<qint64> g_beatLate100{0};
std::atomic<qint64> g_beatLate250{0};
std::atomic<qint64> g_beatLate1000{0};
std::atomic<qint64> g_beatMaxLateNs{0};

std::atomic<const char*> g_phase{"startup"};

std::atomic<qint64> g_stallCount{0};
std::atomic<qint64> g_longestStallNs{0};

// Two flags because the two threads stop at different moments: the watchdog
// before the final records are written, the writer only after them.
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_writerStopping{false};
std::mutex g_stopMutex;
std::mutex g_periodicMutex;
std::condition_variable g_stopCv;
std::thread g_writerThread;
std::thread g_watchdogThread;

std::mutex g_pathMutex;
QString g_logPath;
QString g_logBase;

std::mutex g_sampleMutex;  // serialises resource sampling (the CPU delta is stateful)

QObject* g_beacon = nullptr;

#ifdef Q_OS_WIN
HANDLE g_guiThread = nullptr;
ULONG_PTR g_guiStackHigh = 0;
std::vector<unsigned char> g_stackCopy;
std::mutex g_symMutex;
bool g_symInitialized = false;
#endif

void updateMax(std::atomic<qint64>& target, qint64 value) {
  qint64 current = target.load(std::memory_order_relaxed);
  while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

void appendJsonString(QByteArray& out, const QString& str) {
  out += '"';
  const QByteArray utf8 = str.toUtf8();
  for (const char c : utf8) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          qsnprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

QByteArray jsonNumber(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  return QByteArray::number(value, 'f', 3);
}

quint64 currentThreadId() {
#ifdef Q_OS_WIN
  return ::GetCurrentThreadId();
#else
  return reinterpret_cast<quintptr>(QThread::currentThreadId());
#endif
}

QByteArray beginRecord(const char* type) {
  QByteArray rec;
  rec.reserve(160);
  rec += "{\"t\":";
  rec += jsonNumber(static_cast<double>(nowNs() - g_startNs) / 1e6);
  rec += ",\"ev\":\"";
  rec += type;
  rec += "\",\"th\":\"";
  rec += t_role ? t_role : "other";
  rec += "\",\"tid\":";
  rec += QByteArray::number(currentThreadId());
  return rec;
}

void enqueue(QByteArray&& record) {
  record += "}\n";
  {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.size() >= MAX_QUEUED_RECORDS) {
      g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_queue.push_back(std::move(record));
  }
  g_records.fetch_add(1, std::memory_order_relaxed);
}

void writeRecord(const char* type, const Attr* begin, const Attr* end) {
  if (!enabled()) {
    return;
  }
  // Diagnostics must never be what brings the application down. Building a
  // record allocates, and running out of memory is exactly the situation in
  // which a record is most likely to be written - often from a destructor,
  // where an escaping exception would terminate the process.
  try {
    QByteArray rec = beginRecord(type);
    for (const Attr* a = begin; a != end; ++a) {
      a->appendTo(rec);
    }
    enqueue(std::move(rec));
  } catch (...) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

QByteArray jsonStringArray(const std::vector<QString>& items) {
  QByteArray out = "[";
  bool first = true;
  for (const QString& item : items) {
    if (!first) {
      out += ',';
    }
    first = false;
    appendJsonString(out, item);
  }
  out += ']';
  return out;
}

std::vector<QString> snapshotGuiOps() {
  std::vector<QString> ops;
  const int depth = std::min(g_guiDepth.load(std::memory_order_acquire), MAX_GUI_OPS);
  for (int i = 0; i < depth; ++i) {
    const char* name = g_guiOps[i].load(std::memory_order_relaxed);
    if (name) {
      ops.push_back(QString::fromLatin1(name));
    }
  }
  return ops;
}

/*--------------------------------- Heartbeat ---------------------------------*/

/**
 * Lives on the GUI thread and proves, every 50 ms, that its event loop is still
 * turning. Deliberately its own object: MainWindow::timerEvent() treats any timer
 * delivered to the main window as a request to close it.
 */
class Beacon : public QObject {
 public:
  Beacon() {
    // A precise timer on Windows is a multimedia timer, which holds the whole
    // system at 1 ms timer resolution for as long as it runs - all day, on every
    // operator PC, for a threshold of 250 ms that needs nothing of the kind. Only
    // test runs, which also want exact beat-lateness figures, pay for it.
    m_timer.setTimerType(g_config.level == Level::Verbose ? Qt::PreciseTimer : Qt::CoarseTimer);
    m_timer.setInterval(static_cast<int>(BEAT_INTERVAL_MS));
    QObject::connect(&m_timer, &QTimer::timeout, [this]() { beat(); });
    m_timer.start();
    g_lastBeatNs.store(nowNs(), std::memory_order_release);
  }

 private:
  void beat() {
    const qint64 now = nowNs();
    const qint64 previous = g_lastBeatNs.exchange(now, std::memory_order_acq_rel);
    if (now - previous > 2 * BEAT_INTERVAL_MS * 1000000) {
      // The gap is recorded by the beats themselves, so a stall ends at the first
      // beat after it even when the watchdog only looks later - it may itself be
      // busy capturing a stack, or starved of CPU.
      g_gapEndNs.store(now, std::memory_order_relaxed);
      g_gapStartNs.store(previous, std::memory_order_release);
    }
    if (previous < g_pauseEndNs.load(std::memory_order_acquire)) {
      // The whole process was frozen (a debugger, "suspend process", a sleeping
      // laptop): not lateness of the GUI thread.
      return;
    }
    const qint64 lateNs = now - previous - BEAT_INTERVAL_MS * 1000000;
    g_beatCount.fetch_add(1, std::memory_order_relaxed);
    if (lateNs > 50 * 1000000LL) {
      g_beatLate50.fetch_add(1, std::memory_order_relaxed);
    }
    if (lateNs > 100 * 1000000LL) {
      g_beatLate100.fetch_add(1, std::memory_order_relaxed);
    }
    if (lateNs > 250 * 1000000LL) {
      g_beatLate250.fetch_add(1, std::memory_order_relaxed);
    }
    if (lateNs > 1000 * 1000000LL) {
      g_beatLate1000.fetch_add(1, std::memory_order_relaxed);
    }
    updateMax(g_beatMaxLateNs, lateNs);
  }

  QTimer m_timer;
};

void flushBeatStats() {
  const qint64 n = g_beatCount.exchange(0);
  if (n == 0) {
    return;
  }
  const Attr attrs[] = {Attr("n", n),
                        Attr("late50", g_beatLate50.exchange(0)),
                        Attr("late100", g_beatLate100.exchange(0)),
                        Attr("late250", g_beatLate250.exchange(0)),
                        Attr("late1000", g_beatLate1000.exchange(0)),
                        Attr("max_late", std::max<qint64>(0, g_beatMaxLateNs.exchange(0)) / 1e6)};
  writeRecord("beat", std::begin(attrs), std::end(attrs));
}

/*------------------------------- Stack capture -------------------------------*/

#ifdef Q_OS_WIN
QString describeAddress(DWORD64 address) {
  HMODULE module = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)
      || !module) {
    return QString::fromLatin1("0x%1").arg(address, 0, 16);
  }
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(module, path, MAX_PATH);
  const QString moduleName = QFileInfo(QString::fromWCharArray(path)).fileName();

  {
    std::lock_guard<std::mutex> lock(g_symMutex);
    if (g_crashing.load(std::memory_order_acquire)) {
      // The crash handler is using dbghelp; module+offset is all we can afford.
      const auto base = reinterpret_cast<DWORD64>(module);
      return QString::fromLatin1("%1+0x%2").arg(moduleName).arg(address - base, 0, 16);
    }
    if (!g_symInitialized) {
      // Only the application's own folder and its "symbols" subfolder: never a
      // symbol server, which on an operator's PC would mean network access and a
      // long pause on the first stall.
      const QString exeDir = QCoreApplication::applicationDirPath();
      const QString searchPath = QDir::toNativeSeparators(exeDir) + QLatin1Char(';')
                                 + QDir::toNativeSeparators(exeDir + QLatin1String("/symbols"));
      ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
      g_symInitialized
          = ::SymInitializeW(::GetCurrentProcess(), reinterpret_cast<PCWSTR>(searchPath.utf16()), TRUE) != FALSE;
    }
    if (g_symInitialized) {
      alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
      auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
      symbol->MaxNameLen = 255;
      DWORD64 displacement = 0;
      if (::SymFromAddrW(::GetCurrentProcess(), address, &displacement, symbol)) {
        return QString::fromLatin1("%1!%2+0x%3")
            .arg(moduleName, QString::fromWCharArray(symbol->Name))
            .arg(displacement, 0, 16);
      }
    }
  }
  const auto base = reinterpret_cast<DWORD64>(module);
  return QString::fromLatin1("%1+0x%2").arg(moduleName).arg(address - base, 0, 16);
}

/**
 * \brief Captures the GUI thread's call stack from the watchdog thread.
 *
 * The thread is suspended only long enough to read its registers and copy its
 * stack into a buffer allocated in advance - no allocation, no lock, nothing
 * that could wait on something the suspended thread holds. The unwinding and
 * symbol lookup happen afterwards, on the copy, with pointers into the original
 * stack rewritten to point into the copy. This is the technique sampling
 * profilers use, and the reason it is safe to leave on for everyday use.
 */
std::vector<QString> captureGuiStack() {
  std::vector<QString> frames;
#if defined(_M_X64)
  if (!g_guiThread || g_stackCopy.empty()) {
    return frames;
  }

  CONTEXT context = {};
  context.ContextFlags = CONTEXT_FULL;
  size_t copied = 0;
  DWORD64 originalRsp = 0;

  if (::SuspendThread(g_guiThread) == static_cast<DWORD>(-1)) {
    return frames;
  }
  if (::GetThreadContext(g_guiThread, &context)) {
    originalRsp = context.Rsp;
    if (g_guiStackHigh > originalRsp) {
      copied = std::min<size_t>(static_cast<size_t>(g_guiStackHigh - originalRsp), g_stackCopy.size());
      std::memcpy(g_stackCopy.data(), reinterpret_cast<const void*>(originalRsp), copied);
    }
  }
  ::ResumeThread(g_guiThread);

  if (copied == 0) {
    return frames;
  }

  const auto copyBase = reinterpret_cast<DWORD64>(g_stackCopy.data());
  const DWORD64 originalEnd = originalRsp + copied;
  const auto relocate = [&](DWORD64 value) -> DWORD64 {
    return (value >= originalRsp && value < originalEnd) ? value - originalRsp + copyBase : value;
  };
  for (size_t offset = 0; offset + sizeof(DWORD64) <= copied; offset += sizeof(DWORD64)) {
    auto* slot = reinterpret_cast<DWORD64*>(g_stackCopy.data() + offset);
    *slot = relocate(*slot);
  }
  context.Rsp = relocate(context.Rsp);
  context.Rbp = relocate(context.Rbp);

  std::vector<DWORD64> addresses;
  const DWORD64 copyEnd = copyBase + copied;
  for (int i = 0; i < MAX_STACK_FRAMES && context.Rip != 0; ++i) {
    addresses.push_back(context.Rip);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
    if (function) {
      PVOID handlerData = nullptr;
      DWORD64 establisherFrame = 0;
      ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                         &establisherFrame, nullptr);
    } else {
      // A leaf function: the return address is at the top of the stack.
      if (context.Rsp < copyBase || context.Rsp + sizeof(DWORD64) > copyEnd) {
        break;
      }
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += sizeof(DWORD64);
    }
    if (context.Rsp < copyBase || context.Rsp >= copyEnd) {
      break;
    }
  }

  frames.reserve(addresses.size());
  for (const DWORD64 address : addresses) {
    frames.push_back(describeAddress(address));
  }
#endif
  return frames;
}

QString writeStallDump() {
  const QString path = QDir(g_config.logDir)
                           .absoluteFilePath(QString::fromLatin1("stall-%1-%2.dmp")
                                                 .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"))
                                                 .arg(QCoreApplication::applicationPid()));
  const HANDLE file = ::CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_WRITE,
                                    0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return QString();
  }
  const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
                                               | MiniDumpWithIndirectlyReferencedMemory);
  const BOOL ok
      = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
  ::CloseHandle(file);
  if (!ok) {
    QFile::remove(path);
    return QString();
  }
  return path;
}
#else
std::vector<QString> captureGuiStack() {
  return {};
}

QString writeStallDump() {
  return QString();
}
#endif  // Q_OS_WIN

/*--------------------------------- Watchdog ----------------------------------*/

void watchdogMain() {
  setThreadRole("diag");
  const qint64 thresholdNs = static_cast<qint64>(g_config.stallThresholdMs) * 1000000;
  const qint64 stackNs = static_cast<qint64>(g_config.stackCaptureMs) * 1000000;
  const qint64 dumpNs = static_cast<qint64>(g_config.dumpStallMs) * 1000000;
  int stacksCaptured = 0;
  int dumpsWritten = 0;

  constexpr qint64 PROGRESS_INTERVAL_NS = 10LL * 1000 * 1000000;
  // How long the watchdog's 20 ms wait must overrun before the whole process is
  // taken to have been frozen. Far above anything CPU starvation can cause: a
  // starved watchdog usually means a starved GUI thread, and that is a real stall.
  constexpr qint64 FREEZE_NS = 5LL * 1000 * 1000000;

  bool inStall = false;
  qint64 stallBeat = 0;
  qint64 nextProgressNs = 0;
  qint64 pauseEnd = 0;
  qint64 previousWake = nowNs();
  const char* stallPhase = "run";
  std::vector<QString> opsAtStart;
  std::vector<QString> opsLast;
  std::vector<QString> stack;
  QString dump;

  const auto stallAttrs = [&](qint64 durationNs) {
    std::vector<Attr> attrs;
    attrs.emplace_back("dur", durationNs / 1e6);
    attrs.emplace_back("at", static_cast<double>(stallBeat - g_startNs) / 1e6);
    attrs.emplace_back("phase", stallPhase);
    attrs.push_back(Attr::raw("ops", jsonStringArray(opsAtStart)));
    attrs.push_back(Attr::raw("ops_last", jsonStringArray(opsLast)));
    if (!stack.empty()) {
      attrs.push_back(Attr::raw("stack", jsonStringArray(stack)));
    }
    if (!dump.isEmpty()) {
      attrs.emplace_back("dump", dump);
    }
    return attrs;
  };

  const auto reportStall = [&](qint64 durationNs, bool ongoing) {
    g_stallCount.fetch_add(1, std::memory_order_relaxed);
    updateMax(g_longestStallNs, durationNs);
    std::vector<Attr> attrs = stallAttrs(durationNs);
    if (ongoing) {
      attrs.emplace_back("ongoing", true);
    }
    event("stall", attrs);
  };

  // What is known about a stall that has not ended yet. Without it, a window
  // that never recovers - the operator ends it from Task Manager - would take
  // the stack captured for it to the grave, and leave only a log that stops.
  const auto reportProgress = [&](qint64 durationNs) { event("stall_progress", stallAttrs(durationNs)); };

  std::unique_lock<std::mutex> lock(g_stopMutex);
  while (!g_stopping.load()) {
    g_stopCv.wait_for(lock, std::chrono::milliseconds(20));
    if (g_stopping.load()) {
      break;
    }
    lock.unlock();

    try {
      const qint64 now = nowNs();
      if (now - previousWake > FREEZE_NS) {
        // The watchdog's own 20 ms wait took seconds: the whole process was
        // frozen - a debugger, "suspend process", a laptop in modern standby.
        // That is not the GUI thread failing to respond, so the freeze does not
        // count towards a stall. A stall already under way is reported up to the
        // last moment this thread saw it, not dropped.
        if (inStall) {
          reportStall(previousWake - stallBeat, false);
          inStall = false;
        }
        pauseEnd = now;
        g_pauseEndNs.store(now, std::memory_order_release);
      }

      const qint64 lastBeat = std::max(g_lastBeatNs.load(std::memory_order_acquire), pauseEnd);
      if (g_lastBeatNs.load(std::memory_order_relaxed) != 0) {
        if (!inStall) {
          if (now - lastBeat > thresholdNs) {
            inStall = true;
            stallBeat = lastBeat;
            stallPhase = g_phase.load(std::memory_order_relaxed);
            opsAtStart = snapshotGuiOps();
            opsLast = opsAtStart;
            stack.clear();
            dump.clear();
            nextProgressNs = 0;
          }
        } else if (lastBeat != stallBeat) {
          // Ends at the first beat after the gap, as recorded by the beacon, not
          // at whichever beat is the latest by the time this thread looks.
          qint64 end = lastBeat;
          if (g_gapStartNs.load(std::memory_order_acquire) == stallBeat) {
            const qint64 gapEnd = g_gapEndNs.load(std::memory_order_relaxed);
            if (g_gapStartNs.load(std::memory_order_acquire) == stallBeat) {
              end = gapEnd;
            }
          }
          reportStall(end - stallBeat, false);
          inStall = false;
        } else {
          opsLast = snapshotGuiOps();
          const qint64 gap = now - stallBeat;
          const bool crashing = g_crashing.load(std::memory_order_acquire);
          if (!crashing && stack.empty() && gap > stackNs && stacksCaptured < MAX_STACK_CAPTURES) {
            stack = captureGuiStack();
            ++stacksCaptured;
            nextProgressNs = now;
          }
          if (!crashing && dump.isEmpty() && dumpNs > 0 && gap > dumpNs && dumpsWritten < g_config.maxDumps) {
            dump = writeStallDump();
            ++dumpsWritten;
            nextProgressNs = now;
          }
          if (nextProgressNs != 0 && now >= nextProgressNs) {
            reportProgress(gap);
            nextProgressNs = now + PROGRESS_INTERVAL_NS;
          }
        }
      }
    } catch (...) {
      // One failed allocation must not end stall detection for the session.
    }
    // Taken after this iteration's own work - capturing and symbolising a stack
    // can take a while - so that only the wait itself can look like a freeze.
    previousWake = nowNs();

    lock.lock();
  }
  lock.unlock();

  if (inStall) {
    try {
      reportStall(nowNs() - stallBeat, true);
    } catch (...) {
    }
  }
}

/*------------------------------ Resource sample ------------------------------*/

struct CpuState {
  qint64 lastWallNs = 0;
  qint64 lastCpu100ns = 0;
};

CpuState g_cpuState;

void appendResources(std::vector<Attr>& attrs) {
#ifdef Q_OS_WIN
  const HANDLE process = ::GetCurrentProcess();

  PROCESS_MEMORY_COUNTERS_EX memory = {};
  memory.cb = sizeof(memory);
  if (::K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
    constexpr double MB = 1024.0 * 1024.0;
    attrs.emplace_back("private_mb", memory.PrivateUsage / MB);
    attrs.emplace_back("ws_mb", memory.WorkingSetSize / MB);
    attrs.emplace_back("peak_ws_mb", memory.PeakWorkingSetSize / MB);
  }

  DWORD handles = 0;
  if (::GetProcessHandleCount(process, &handles)) {
    attrs.emplace_back("handles", static_cast<qint64>(handles));
  }
  attrs.emplace_back("gdi", static_cast<qint64>(::GetGuiResources(process, GR_GDIOBJECTS)));
  attrs.emplace_back("user", static_cast<qint64>(::GetGuiResources(process, GR_USEROBJECTS)));

  const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    const DWORD pid = ::GetCurrentProcessId();
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    qint64 threads = 0;
    if (::Thread32First(snapshot, &entry)) {
      do {
        if (entry.th32OwnerProcessID == pid) {
          ++threads;
        }
      } while (::Thread32Next(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    attrs.emplace_back("threads", threads);
  }

  FILETIME creation, exit, kernel, user;
  if (::GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
    const auto toInt = [](const FILETIME& ft) {
      return (static_cast<qint64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const qint64 cpu100ns = toInt(kernel) + toInt(user);
    const qint64 wallNs = nowNs();
    attrs.emplace_back("cpu_s", cpu100ns / 1e7);
    if (g_cpuState.lastWallNs != 0 && wallNs > g_cpuState.lastWallNs) {
      SYSTEM_INFO info;
      ::GetSystemInfo(&info);
      const double cores = std::max<DWORD>(1, info.dwNumberOfProcessors);
      const double cpuNs = (cpu100ns - g_cpuState.lastCpu100ns) * 100.0;
      attrs.emplace_back("cpu_pct", 100.0 * cpuNs / ((wallNs - g_cpuState.lastWallNs) * cores));
    }
    g_cpuState.lastWallNs = wallNs;
    g_cpuState.lastCpu100ns = cpu100ns;
  }

  IO_COUNTERS io = {};
  if (::GetProcessIoCounters(process, &io)) {
    constexpr double MB = 1024.0 * 1024.0;
    attrs.emplace_back("io_read_mb", io.ReadTransferCount / MB);
    attrs.emplace_back("io_write_mb", io.WriteTransferCount / MB);
    attrs.emplace_back("io_other_mb", io.OtherTransferCount / MB);
    attrs.emplace_back("io_read_ops", static_cast<qint64>(io.ReadOperationCount));
    attrs.emplace_back("io_write_ops", static_cast<qint64>(io.WriteOperationCount));
  }

  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (::GlobalMemoryStatusEx(&status)) {
    constexpr double MB = 1024.0 * 1024.0;
    attrs.emplace_back("sys_avail_mb", status.ullAvailPhys / MB);
    attrs.emplace_back("sys_load_pct", static_cast<qint64>(status.dwMemoryLoad));
    attrs.emplace_back("sys_commit_mb", (status.ullTotalPageFile - status.ullAvailPageFile) / MB);
    attrs.emplace_back("sys_commit_limit_mb", status.ullTotalPageFile / MB);
  }
#else
  Q_UNUSED(attrs);
#endif
}

void writeSample(const char* reason, std::initializer_list<Attr> extra) {
  // Called from the out-of-memory handler among other places: see writeRecord().
  try {
    std::vector<Attr> attrs;
    attrs.emplace_back("reason", reason);
    for (const Attr& a : extra) {
      attrs.push_back(a);
    }
    {
      const std::lock_guard<std::mutex> lock(g_sampleMutex);
      appendResources(attrs);
    }
    {
      const std::lock_guard<std::mutex> lock(g_queueMutex);
      attrs.emplace_back("log_queue", static_cast<qint64>(g_queue.size()));
    }
    attrs.emplace_back("log_dropped", g_dropped.load());
    event("res", attrs);
  } catch (...) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

/*---------------------------------- Writer -----------------------------------*/

class LogFile {
 public:
  bool write(const std::vector<QByteArray>& records) {
    if (records.empty()) {
      return true;
    }
    if (!ensureOpen()) {
      g_dropped.fetch_add(static_cast<qint64>(records.size()), std::memory_order_relaxed);
      return false;
    }
    bool ok = true;
    for (const QByteArray& rec : records) {
      if (m_file.write(rec) != rec.size()) {
        ok = false;
        break;
      }
      m_size += rec.size();
    }
    // A buffered QFile only reports an I/O error here.
    if (!m_file.flush()) {
      ok = false;
    }
    if (!ok) {
      // Most likely a log folder on a share whose connection dropped: the handle
      // is dead. Close it so the next batch reopens the file, rather than losing
      // everything for the rest of the session.
      m_file.close();
      g_dropped.fetch_add(static_cast<qint64>(records.size()), std::memory_order_relaxed);
      return false;
    }
    if (m_size >= MAX_FILE_BYTES) {
      m_file.close();
      ++m_part;
    }
    return true;
  }

  void close() { m_file.close(); }

 private:
  bool ensureOpen() {
    if (m_file.isOpen()) {
      return true;
    }
    QString path;
    {
      const std::lock_guard<std::mutex> lock(g_pathMutex);
      path = (m_part == 1) ? g_logBase + QLatin1String(".jsonl")
                           : g_logBase + QString::fromLatin1(".%1.jsonl").arg(m_part);
      g_logPath = path;
    }
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
      return false;
    }
    // A reopened part already holds bytes that count towards its size limit.
    m_size = m_file.size();
    return true;
  }

  QFile m_file;
  qint64 m_size = 0;
  int m_part = 1;
};

void removeOldLogs() {
  if (g_config.keepDays <= 0) {
    return;
  }
  const QDateTime cutoff = QDateTime::currentDateTime().addDays(-g_config.keepDays);
  const QDir dir(g_config.logDir);
  const QStringList patterns{QStringLiteral("scantailor-perf-*.jsonl"), QStringLiteral("stall-*.dmp")};
  for (const QFileInfo& info : dir.entryInfoList(patterns, QDir::Files)) {
    if (info.lastModified() < cutoff) {
      QFile::remove(info.absoluteFilePath());
    }
  }
}

void writerMain() {
  setThreadRole("diag");
  removeOldLogs();

  const int sampleMs = (g_config.sampleIntervalMs > 0) ? g_config.sampleIntervalMs
                                                        : (g_config.level == Level::Verbose ? 1000 : 30000);
  LogFile file;
  qint64 nextSample = nowNs() + static_cast<qint64>(sampleMs) * 1000000;
  std::vector<QByteArray> batch;

  while (true) {
    const bool stopping = g_writerStopping.load();
    {
      std::unique_lock<std::mutex> lock(g_queueMutex);
      if (!stopping && g_queue.empty()) {
        g_queueCv.wait_for(lock, std::chrono::milliseconds(250));
      }
      batch.swap(g_queue);
    }
    if (!batch.empty()) {
      file.write(batch);
      batch.clear();
    }
    if (stopping) {
      break;
    }
    if (nowNs() >= nextSample) {
      nextSample = nowNs() + static_cast<qint64>(sampleMs) * 1000000;
      // Under the lock stop() takes before writing the final records, so no
      // periodic sample can land after the "stop" record.
      const std::lock_guard<std::mutex> lock(g_periodicMutex);
      if (!g_stopping.load()) {
        writeSample("periodic", {});
        flushBeatStats();
        flushCounters();
      }
    }
  }

  file.close();
}

void writeStartRecords() {
  std::vector<Attr> attrs;
  attrs.emplace_back("wall", QDateTime::currentDateTime().toOffsetFromUtc(QDateTime::currentDateTime().offsetFromUtc())
                                 .toString(Qt::ISODate));
  attrs.emplace_back("pid", static_cast<qint64>(QCoreApplication::applicationPid()));
  attrs.emplace_back("app", QCoreApplication::applicationName());
  attrs.emplace_back("version", QString::fromUtf8(VERSION));
  attrs.emplace_back("level", g_config.level == Level::Verbose ? "verbose" : "basic");
  attrs.emplace_back("exe", QCoreApplication::applicationFilePath());
  attrs.emplace_back("log_dir", g_config.logDir);
  attrs.emplace_back("os", QSysInfo::prettyProductName());
  attrs.emplace_back("os_kernel", QSysInfo::kernelVersion());
  attrs.emplace_back("qt", QString::fromLatin1(qVersion()));
  attrs.emplace_back("portable", !QString::fromUtf8(PORTABLE_CONFIG_DIR).isEmpty());
#ifdef Q_OS_WIN
  const QSettings cpu(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"),
                      QSettings::NativeFormat);
  attrs.emplace_back("cpu", cpu.value(QStringLiteral("ProcessorNameString")).toString().trimmed());
  SYSTEM_INFO info;
  ::GetSystemInfo(&info);
  attrs.emplace_back("cores", static_cast<qint64>(info.dwNumberOfProcessors));
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (::GlobalMemoryStatusEx(&status)) {
    attrs.emplace_back("ram_mb", static_cast<qint64>(status.ullTotalPhys / (1024 * 1024)));
  }
#endif
  event("start", attrs);

  if (auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
      const QScreen* screen = screens[i];
      event("screen", {Attr("index", i), Attr("name", screen->name()),
                       Attr("primary", screen == QGuiApplication::primaryScreen()),
                       Attr("w", screen->geometry().width()), Attr("h", screen->geometry().height()),
                       Attr("dpr", screen->devicePixelRatio()), Attr("logical_dpi", screen->logicalDotsPerInch()),
                       Attr("physical_dpi", screen->physicalDotsPerInch()),
                       Attr("refresh_hz", screen->refreshRate())});
    }
    Q_UNUSED(guiApp);
  }
}
}  // namespace

/*------------------------------- Public API ---------------------------------*/

qint64 nowNs() {
#ifdef Q_OS_WIN
  // The Precise variant: the plain one only advances once per clock tick, up to
  // 15.6 ms, which would round every short operation to zero or one tick. It is
  // exported by KernelBase but has no import library in the default link set,
  // hence the lookup.
  using PreciseFn = VOID(WINAPI*)(PULONGLONG);
  static const PreciseFn precise = []() -> PreciseFn {
    const HMODULE kernelBase = ::GetModuleHandleW(L"kernelbase.dll");
    return kernelBase ? reinterpret_cast<PreciseFn>(::GetProcAddress(kernelBase, "QueryUnbiasedInterruptTimePrecise"))
                      : nullptr;
  }();
  ULONGLONG unbiased = 0;
  if (precise) {
    precise(&unbiased);
  } else {
    ::QueryUnbiasedInterruptTime(&unbiased);
  }
  return static_cast<qint64>(unbiased) * 100;
#else
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}

Level parseLevel(const QString& text, const Level fallback) {
  const QString value = text.trimmed().toLower();
  if (value == QLatin1String("off") || value == QLatin1String("0")) {
    return Level::Off;
  }
  if (value == QLatin1String("basic") || value == QLatin1String("1")) {
    return Level::Basic;
  }
  if (value == QLatin1String("verbose") || value == QLatin1String("2")) {
    return Level::Verbose;
  }
  return fallback;
}

Level level() {
  return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

QString logFilePath() {
  const std::lock_guard<std::mutex> lock(g_pathMutex);
  return g_logPath;
}

void start(const Config& config) {
  if (enabled() || g_writerThread.joinable()) {
    return;
  }
  g_config = config;
  const QByteArray env = qgetenv("SCANTAILOR_DIAG");
  if (!env.isEmpty()) {
    g_config.level = parseLevel(QString::fromLatin1(env), g_config.level);
  }
  if (g_config.level == Level::Off || g_config.logDir.isEmpty() || !QDir().mkpath(g_config.logDir)) {
    return;
  }

  g_startNs = nowNs();
  g_stopping.store(false);
  g_writerStopping.store(false);
  g_phase.store("startup");
  {
    const std::lock_guard<std::mutex> lock(g_pathMutex);
    g_logBase = QDir(g_config.logDir)
                    .absoluteFilePath(QString::fromLatin1("scantailor-perf-%1-%2")
                                          .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"))
                                          .arg(QCoreApplication::applicationPid()));
    g_logPath = g_logBase + QLatin1String(".jsonl");
  }

  setThreadRole("gui");
  t_isGui = true;
#ifdef Q_OS_WIN
  ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &g_guiThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
  // From the thread information block rather than GetCurrentThreadStackLimits(),
  // which would make the executable require Windows 8 just to load.
  g_guiStackHigh = reinterpret_cast<ULONG_PTR>(reinterpret_cast<const NT_TIB*>(::NtCurrentTeb())->StackBase);
  g_stackCopy.resize(STACK_COPY_BYTES);
#endif

  g_level.store(static_cast<int>(g_config.level));
  writeStartRecords();

  g_beacon = new Beacon();
  // An exception escaping a thread function terminates the process, so each
  // thread ends quietly instead. Diagnostics then degrade; the application does not.
  g_writerThread = std::thread([]() {
    try {
      writerMain();
    } catch (...) {
    }
  });
  g_watchdogThread = std::thread([]() {
    try {
      watchdogMain();
    } catch (...) {
    }
  });
}

void stop() {
  if (!g_writerThread.joinable()) {
    return;
  }

  // The event loop has normally stopped by now, so the heartbeat has too. Stop
  // the watchdog first, or the shutdown itself would be reported as a stall.
  delete g_beacon;
  g_beacon = nullptr;
  {
    const std::lock_guard<std::mutex> periodicLock(g_periodicMutex);
    const std::lock_guard<std::mutex> lock(g_stopMutex);
    g_stopping.store(true);
  }
  g_stopCv.notify_all();
  g_watchdogThread.join();

  // Whatever happens here, the writer must still be told to stop and be joined.
  try {
    writeSample("final", {});
    flushBeatStats();
    flushCounters();
    event("stop", {Attr("stalls", g_stallCount.load()), Attr("longest_stall", g_longestStallNs.load() / 1e6),
                   Attr("records", g_records.load()), Attr("dropped", g_dropped.load())});
  } catch (...) {
  }

  // Only now: the writer's last pass must see every record above.
  g_writerStopping.store(true);
  g_queueCv.notify_all();
  g_writerThread.join();
  g_level.store(static_cast<int>(Level::Off));

#ifdef Q_OS_WIN
  if (g_guiThread) {
    ::CloseHandle(g_guiThread);
    g_guiThread = nullptr;
  }
  {
    const std::lock_guard<std::mutex> lock(g_symMutex);
    if (g_symInitialized) {
      ::SymCleanup(::GetCurrentProcess());
      g_symInitialized = false;
    }
  }
#endif
}

void setThreadRole(const char* name) {
  t_role = name;
}

void setPhase(const char* phase) {
  g_phase.store(phase, std::memory_order_relaxed);
  // A phase change also counts as a heartbeat (it is always made on the GUI
  // thread), so a stall that began in the previous phase ends here and the time
  // after the boundary is attributed to the new one. Otherwise, opening a title
  // named on the command line - before the event loop, so before any beat -
  // would be filed under start-up and left out of the verdict.
  if (g_beacon) {
    g_lastBeatNs.store(nowNs(), std::memory_order_release);
  }
  event("phase", {Attr("phase", phase)});
}

void notifyCrashing() noexcept {
  g_crashing.store(true, std::memory_order_release);
}

quint64 beginTask() {
  t_task = g_nextTask.fetch_add(1, std::memory_order_relaxed);
  return t_task;
}

void endTask() {
  t_task = 0;
}

void sampleNow(const char* reason, std::initializer_list<Attr> attrs) {
  if (enabled()) {
    writeSample(reason, attrs);
  }
}

/*----------------------------------- Attr ------------------------------------*/

Attr Attr::raw(const char* key, const QByteArray& json) {
  Attr attr(key, 0);
  attr.m_type = Raw;
  attr.m_raw = json;
  return attr;
}

void Attr::appendTo(QByteArray& json) const {
  json += ",\"";
  json += m_key;
  json += "\":";
  switch (m_type) {
    case Int:
      json += QByteArray::number(m_int);
      break;
    case Double:
      json += jsonNumber(m_double);
      break;
    case Bool:
      json += m_int ? "true" : "false";
      break;
    case String:
      appendJsonString(json, m_string);
      break;
    case Raw:
      json += m_raw.isEmpty() ? QByteArray("null") : m_raw;
      break;
  }
}

void event(const char* type, std::initializer_list<Attr> attrs) {
  writeRecord(type, attrs.begin(), attrs.end());
}

void event(const char* type, const std::vector<Attr>& attrs) {
  writeRecord(type, attrs.data(), attrs.data() + attrs.size());
}

/*---------------------------------- Scope ------------------------------------*/

Scope::Scope(const char* name, Counter* aggregate)
    : m_name(name), m_aggregate(aggregate), m_startNs(0), m_active(enabled()) {
  if (!m_active) {
    return;
  }
  m_startNs = nowNs();
  const int depth = t_depth++;
  if (t_isGui && depth < MAX_GUI_OPS) {
    g_guiOps[depth].store(name, std::memory_order_relaxed);
    g_guiDepth.store(depth + 1, std::memory_order_release);
    m_onGuiStack = true;
  }
}

Scope::~Scope() {
  if (!m_active) {
    return;
  }
  const qint64 durationNs = nowNs() - m_startNs;
  const int depth = t_depth--;
  if (m_onGuiStack) {
    g_guiDepth.store(depth - 1, std::memory_order_release);
  }
  if (m_aggregate) {
    m_aggregate->add(durationNs);
  }

  const qint64 slowMs = t_isGui ? g_config.guiSlowOpMs : g_config.otherSlowOpMs;
  if (!(m_force || verbose() || durationNs >= slowMs * 1000000)) {
    return;
  }
  // See writeRecord(): a destructor must not let an allocation failure escape.
  try {
    QByteArray rec = beginRecord("op");
    rec += ",\"name\":\"";
    rec += m_name;
    rec += "\",\"dur\":";
    rec += jsonNumber(durationNs / 1e6);
    rec += ",\"depth\":";
    rec += QByteArray::number(depth);
    rec += ",\"task\":";
    rec += QByteArray::number(t_task);
    for (const Attr& a : m_attrs) {
      a.appendTo(rec);
    }
    enqueue(std::move(rec));
  } catch (...) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void Scope::attr(const Attr& attr) {
  if (m_active) {
    try {
      m_attrs.push_back(attr);
    } catch (...) {
      // Losing an attribute is better than losing the operation.
    }
  }
}

double Scope::elapsedMs() const {
  return m_active ? (nowNs() - m_startNs) / 1e6 : 0.0;
}

/*--------------------------------- Counters ----------------------------------*/

Counter::Counter(const char* name) : m_name(name) {
  m_next = g_counters.load(std::memory_order_relaxed);
  while (!g_counters.compare_exchange_weak(m_next, this, std::memory_order_release, std::memory_order_relaxed)) {
  }
}

void Counter::add(const qint64 durationNs) {
  m_count.fetch_add(1, std::memory_order_relaxed);
  m_totalNs.fetch_add(durationNs, std::memory_order_relaxed);
  updateMax(m_maxNs, durationNs);
}

void flushCounters() {
  if (!enabled()) {
    return;
  }
  for (Counter* c = g_counters.load(std::memory_order_acquire); c; c = c->m_next) {
    const qint64 n = c->m_count.exchange(0, std::memory_order_relaxed);
    if (n == 0) {
      continue;
    }
    const qint64 total = c->m_totalNs.exchange(0, std::memory_order_relaxed);
    const qint64 maximum = c->m_maxNs.exchange(0, std::memory_order_relaxed);
    try {
      const Attr attrs[] = {Attr("name", c->m_name), Attr("n", n), Attr("sum", total / 1e6),
                            Attr("max", maximum / 1e6)};
      writeRecord("agg", std::begin(attrs), std::end(attrs));
    } catch (...) {
      g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

CounterScope::CounterScope(Counter& counter) : m_counter(counter), m_startNs(enabled() ? nowNs() : 0) {}

CounterScope::~CounterScope() {
  if (m_startNs != 0) {
    m_counter.add(nowNs() - m_startNs);
  }
}
}  // namespace diag
}  // namespace core
