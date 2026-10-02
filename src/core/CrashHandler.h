// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_CRASHHANDLER_H_
#define SCANTAILOR_CORE_CRASHHANDLER_H_

#include <QString>

namespace core {
/**
 * \brief Process-wide crash diagnostics.
 *
 * Historically ScanTailor left no trace whatsoever when it died: a worker thread
 * letting an exception escape terminates the process silently, and because the
 * application is built as a GUI (WIN32) binary, qWarning() output goes nowhere.
 * That makes field reports of the "it just closes" kind impossible to diagnose.
 *
 * This class installs, as early as possible in main():
 *  - a Qt message handler that mirrors qDebug/qWarning/qCritical to a log file;
 *  - an unhandled SEH exception filter that writes a minidump (Windows);
 *  - a std::terminate handler, which is what actually fires when an exception
 *    escapes a worker thread - the single most likely cause of a silent exit;
 *  - handlers for abort(), pure virtual calls and invalid CRT parameters.
 *
 * None of this changes application behaviour: it only makes a crash leave
 * evidence behind. Data preservation is handled separately by the autosave and
 * atomic project-writing machinery.
 */
class CrashHandler {
 public:
  /**
   * \brief Installs the handlers. Safe to call once; later calls are ignored.
   *
   * \param reportDir Directory to write logs and minidumps to. It is created if
   *        missing. If empty or not writable, a subdirectory of the system
   *        temporary directory is used instead.
   */
  static void install(const QString& reportDir);

  /**
   * \brief The directory reports are actually being written to.
   *
   * Empty if install() has not been called or no writable location was found.
   */
  static QString reportDir();

  /**
   * \brief Path of the log file, or an empty string if logging is not active.
   */
  static QString logFilePath();

  /**
   * \brief Records the project currently open, so it appears in crash reports.
   *
   * Knowing which project (and therefore which batch of scans) was open when
   * the process died is usually the first thing needed to reproduce a report.
   */
  static void setCurrentProjectFile(const QString& projectFilePath);

  /**
   * \brief Writes a line to the log file. No-op if logging is not active.
   */
  static void log(const QString& message);

  CrashHandler() = delete;
};
}  // namespace core

#endif  // ifndef SCANTAILOR_CORE_CRASHHANDLER_H_
