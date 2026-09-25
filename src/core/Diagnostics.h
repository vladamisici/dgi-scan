// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_DIAGNOSTICS_H_
#define SCANTAILOR_CORE_DIAGNOSTICS_H_

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <initializer_list>
#include <vector>

namespace core {
/**
 * \brief Performance and resource diagnostics, written as JSON lines.
 *
 * Exists to answer "where does the time go on this particular PC" with data
 * rather than guesses. It records:
 *
 *  - every stall of the GUI thread above a threshold, with the operations that
 *    were open on it at the time and, for long ones, the native call stack;
 *  - the duration of the operations that matter (saving, loading, stage
 *    processing, file replacement, thumbnail work), by thread;
 *  - a periodic sample of the process's memory, handles, GDI/USER objects,
 *    threads, CPU and disk I/O, which is what leak detection is built on.
 *
 * Producers never touch the disk. Records are queued in memory and written by a
 * dedicated thread, so a slow log location cannot itself become the stall being
 * measured, and a stall of the GUI thread cannot stop the watchdog reporting it.
 *
 * The file format is documented in diagnostics/SCHEMA.md.
 */
namespace diag {
enum class Level {
  /** Nothing is recorded and no thread is started. */
  Off = 0,
  /**
   * The default for everyday use. Stalls, slow operations only, and a resource
   * sample every half minute: cheap enough to leave on for a whole working day.
   */
  Basic = 1,
  /** Every instrumented operation and a sample every second. For test runs. */
  Verbose = 2,
};

struct Config {
  Level level = Level::Basic;
  /** Where the log files go. Created if missing. */
  QString logDir;
  /** A GUI thread unresponsive for this long is recorded as a stall. */
  int stallThresholdMs = 250;
  /** The native stack of the GUI thread is captured once a stall lasts this long. */
  int stackCaptureMs = 1000;
  /**
   * A minidump is written once a stall lasts this long; 0 disables it.
   *
   * Off by default: writing a dump of the process from inside it suspends every
   * thread, and should that ever wedge, the operator's window would hang for
   * good. Test runs turn it on; everyday use relies on the stack capture, which
   * suspends only the GUI thread and only for the length of a memory copy.
   */
  int dumpStallMs = 0;
  /** At most this many stall minidumps per process. */
  int maxDumps = 2;
  /** Interval between resource samples; 0 picks a default for the level. */
  int sampleIntervalMs = 0;
  /**
   * In Basic mode, operations shorter than these are not written individually.
   * Everything is still counted in the periodic aggregates.
   */
  int guiSlowOpMs = 50;
  int otherSlowOpMs = 2000;
  /** Log files older than this many days are deleted at start. */
  int keepDays = 14;
};

/**
 * \brief Starts diagnostics. Call once, on the GUI thread, after QApplication exists.
 *
 * The level can be overridden without a rebuild through the SCANTAILOR_DIAG
 * environment variable (off, basic or verbose).
 */
void start(const Config& config);

/** \brief Writes the final records, flushes and stops the background threads. */
void stop();

Level level();

inline bool enabled() {
  return level() != Level::Off;
}

inline bool verbose() {
  return level() == Level::Verbose;
}

/** \brief Path of the current log file, or empty when diagnostics are off. */
QString logFilePath();

/** \brief Parses "off", "basic" or "verbose"; anything else yields \p fallback. */
Level parseLevel(const QString& text, Level fallback);

/**
 * \brief One attribute of a record: a key and a number, a flag or a string.
 *
 * Keys must be string literals, or otherwise outlive the record.
 */
class Attr {
 public:
  Attr(const char* key, int value) : m_key(key), m_type(Int), m_int(value) {}
  Attr(const char* key, qint64 value) : m_key(key), m_type(Int), m_int(value) {}
  Attr(const char* key, quint64 value) : m_key(key), m_type(Int), m_int(static_cast<qint64>(value)) {}
  Attr(const char* key, double value) : m_key(key), m_type(Double), m_double(value) {}
  Attr(const char* key, bool value) : m_key(key), m_type(Bool), m_int(value ? 1 : 0) {}
  Attr(const char* key, const char* value) : m_key(key), m_type(String), m_string(QString::fromUtf8(value)) {}
  Attr(const char* key, const QString& value) : m_key(key), m_type(String), m_string(value) {}

  /** \brief An attribute whose value is already valid JSON, such as an array. */
  static Attr raw(const char* key, const QByteArray& json);

  /** \brief Appends `,"key":value` to \p json. */
  void appendTo(QByteArray& json) const;

 private:
  enum Type { Int, Double, Bool, String, Raw };

  const char* m_key;
  Type m_type;
  qint64 m_int = 0;
  double m_double = 0;
  QString m_string;
  QByteArray m_raw;
};

/** \brief Writes a one-off record of type \p type. */
void event(const char* type, std::initializer_list<Attr> attrs = {});

/** \brief As event(), for attributes built at run time. */
void event(const char* type, const std::vector<Attr>& attrs);

/**
 * \brief Accumulates a high-frequency operation instead of recording each call.
 *
 * Meant to be a function-local static, which is what DIAG_COUNT and DIAG_SCOPE
 * declare. The totals are written as "agg" records with every resource sample.
 */
class Counter {
 public:
  explicit Counter(const char* name);

  void add(qint64 durationNs);

  const char* name() const { return m_name; }

 private:
  friend void flushCounters();

  const char* m_name;
  std::atomic<qint64> m_count{0};
  std::atomic<qint64> m_totalNs{0};
  std::atomic<qint64> m_maxNs{0};
  Counter* m_next = nullptr;
};

/** \brief Writes an "agg" record for every counter used since the previous call, and resets them. */
void flushCounters();

/**
 * \brief Times an operation from construction to destruction.
 *
 * Operation names follow `area.op[.phase]`, are string literals, and never carry
 * data - that goes in attributes. Scopes nest; each record carries its depth and
 * the id of the background task it ran under, which is how the analyser tells a
 * stage's own time from the time of the stages it hands on to.
 *
 * On the GUI thread, the names of the open scopes are also what a stall record
 * reports as "what the GUI thread was doing".
 */
class Scope {
 public:
  /**
   * \param aggregate Receives this scope's duration even when the record itself
   *        is not written. DIAG_SCOPE supplies one per call site.
   */
  explicit Scope(const char* name, Counter* aggregate = nullptr);

  ~Scope();

  Scope(const Scope&) = delete;

  Scope& operator=(const Scope&) = delete;

  void attr(const Attr& attr);

  double elapsedMs() const;

  /** \brief Always write this record, whatever its duration and the level. */
  void forceRecord() { m_force = true; }

 private:
  const char* m_name;
  Counter* m_aggregate;
  qint64 m_startNs;
  std::vector<Attr> m_attrs;
  bool m_active;
  bool m_onGuiStack = false;
  bool m_force = false;
};

/** \brief Adds the lifetime of this object to a Counter. */
class CounterScope {
 public:
  explicit CounterScope(Counter& counter);

  ~CounterScope();

  CounterScope(const CounterScope&) = delete;

  CounterScope& operator=(const CounterScope&) = delete;

 private:
  Counter& m_counter;
  qint64 m_startNs;
};

/**
 * \brief Labels the calling thread in every record it produces ("gui", "worker", ...).
 *
 * \p name must be a string literal.
 */
void setThreadRole(const char* name);

/**
 * \brief Marks the calling thread as working on a new background task.
 *
 * Every record the thread writes until endTask() carries the returned id, which
 * is unique within the process.
 */
quint64 beginTask();

void endTask();

/** \brief Monotonic nanoseconds, excluding time the machine spent asleep. */
qint64 nowNs();

/**
 * \brief Names the phase the application is in: "startup", "run" or "shutdown".
 *
 * Every stall record carries the phase it began in. A GUI thread that is busy
 * building the main window or tearing it down is not one the operator is
 * waiting on, and a report should be able to tell those apart. \p phase must be
 * a string literal.
 */
void setPhase(const char* phase);

/**
 * \brief Tells diagnostics a crash report is being written.
 *
 * dbghelp is single-threaded, so the watchdog stops using it from then on.
 * Safe from a crash handler: no allocation, no lock.
 */
void notifyCrashing() noexcept;

/**
 * \brief Takes a resource sample right now and writes it as a "res" record.
 *
 * \p reason is written with it ("periodic", "cycle_end", ...). Safe from any thread.
 */
void sampleNow(const char* reason, std::initializer_list<Attr> attrs = {});
}  // namespace diag
}  // namespace core

#define SCANTAILOR_DIAG_CONCAT_IMPL(a, b) a##b
#define SCANTAILOR_DIAG_CONCAT(a, b) SCANTAILOR_DIAG_CONCAT_IMPL(a, b)

/**
 * Times the rest of the enclosing block as operation \p name, in a Scope called
 * \p var. The call site gets its own aggregate counter, so Basic mode still
 * accounts for every call without writing each one.
 */
#define DIAG_SCOPE(var, name)                                                   \
  static ::core::diag::Counter SCANTAILOR_DIAG_CONCAT(var, DiagAggregate)(name); \
  ::core::diag::Scope var(name, &SCANTAILOR_DIAG_CONCAT(var, DiagAggregate))

/** Adds the rest of the enclosing block to the named, function-local aggregate counter. */
#define DIAG_COUNT(name)                                                                  \
  static ::core::diag::Counter SCANTAILOR_DIAG_CONCAT(diagCounter_, __LINE__)(name);      \
  const ::core::diag::CounterScope SCANTAILOR_DIAG_CONCAT(diagCounterScope_, __LINE__)( \
      SCANTAILOR_DIAG_CONCAT(diagCounter_, __LINE__))

#endif  // ifndef SCANTAILOR_CORE_DIAGNOSTICS_H_
