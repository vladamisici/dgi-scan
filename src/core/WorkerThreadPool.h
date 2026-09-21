// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_WORKERTHREADPOOL_H_
#define SCANTAILOR_CORE_WORKERTHREADPOOL_H_

#include <QObject>
#include <QSettings>
#include <memory>

#include "BackgroundTask.h"
#include "FilterResult.h"

class QThreadPool;

class WorkerThreadPool : public QObject {
  Q_OBJECT
 public:
  explicit WorkerThreadPool(QObject* parent = nullptr);

  ~WorkerThreadPool() override;

  /**
   * \brief Waits for pending jobs to finish and stop the thread.
   *
   * The destructor also performs these tasks, so this method is only
   * useful to prematuraly stop task processing.
   */
  void shutdown();

  /**
   * \brief Whether every worker has finished.
   *
   * Lets the GUI thread find out that outstanding tasks are done without
   * waiting for them. shutdown() answers the same question by blocking, which
   * is only acceptable while the application is already on its way out.
   */
  bool isIdle() const;

  bool hasSpareCapacity() const;

  void submitTask(const BackgroundTaskPtr& task);

 signals:

  void taskResult(const BackgroundTaskPtr& task, const FilterResultPtr& result);

 private:
  class TaskResultEvent;

  void customEvent(QEvent* event) override;

  void updateNumberOfThreads();

  QThreadPool* m_pool;
  QSettings m_settings;
};


#endif  // ifndef SCANTAILOR_CORE_WORKERTHREADPOOL_H_
