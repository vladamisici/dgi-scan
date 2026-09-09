// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "WorkerThreadPool.h"

#include <QCoreApplication>
#include <QDebug>
#include <QThreadPool>
#include <utility>

#include "OutOfMemoryHandler.h"

class WorkerThreadPool::TaskResultEvent : public QEvent {
 public:
  TaskResultEvent(BackgroundTaskPtr task, FilterResultPtr result)
      : QEvent(User), m_task(std::move(task)), m_result(std::move(result)) {}

  const BackgroundTaskPtr& task() const { return m_task; }

  const FilterResultPtr& result() const { return m_result; }

 private:
  BackgroundTaskPtr m_task;
  FilterResultPtr m_result;
};


WorkerThreadPool::WorkerThreadPool(QObject* parent) : QObject(parent), m_pool(new QThreadPool(this)) {
  updateNumberOfThreads();
}

WorkerThreadPool::~WorkerThreadPool() = default;

void WorkerThreadPool::shutdown() {
  // Drop everything that has not started yet, then wait with a bound. The untimed
  // wait used here before could park the GUI thread for as long as the slowest
  // output-generation step took, during which the window stops repainting and
  // Windows offers to kill the "not responding" application - turning an orderly
  // close into exactly the abrupt termination we are trying to eliminate.
  m_pool->clear();
  if (!m_pool->waitForDone(15000)) {
    qWarning() << "Worker thread pool did not finish within 15 seconds; shutting down anyway";
  }
}

bool WorkerThreadPool::hasSpareCapacity() const {
  return m_pool->activeThreadCount() < m_pool->maxThreadCount();
}

void WorkerThreadPool::submitTask(const BackgroundTaskPtr& task) {
  class Runnable : public QRunnable {
   public:
    Runnable(WorkerThreadPool& owner, BackgroundTaskPtr task) : m_owner(owner), m_task(std::move(task)) {
      setAutoDelete(true);
    }

    void run() override {
      if (m_task->isCancelled()) {
        return;
      }

      try {
        const FilterResultPtr result((*m_task)());
        if (result) {
          QCoreApplication::postEvent(&m_owner, new TaskResultEvent(m_task, result));
        }
      } catch (const std::bad_alloc&) {
        OutOfMemoryHandler::instance().handleOutOfMemorySituation();
        reportFailure();
      } catch (const std::exception& e) {
        // Until these handlers existed, anything other than bad_alloc escaped
        // QRunnable::run() and reached std::terminate, which kills the process
        // outright - no dialog, no chance to save. Dropping one page's result
        // costs that page a reprocess; letting the exception through cost the
        // operator their entire session.
        qCritical() << "Background task failed:" << e.what();
        reportFailure();
      } catch (...) {
        qCritical() << "Background task failed with an unknown exception";
        reportFailure();
      }
    }

   private:
    /**
     * A failed task must still be reported, with a null result. The queues are
     * only ever advanced from MainWindow::filterResult(), which this event is
     * what triggers - so staying silent here would leave the task's entry in the
     * queue forever, and batch processing would stop dead on the first page that
     * threw rather than carrying on to the rest of the title.
     */
    void reportFailure() { QCoreApplication::postEvent(&m_owner, new TaskResultEvent(m_task, FilterResultPtr())); }

    WorkerThreadPool& m_owner;
    BackgroundTaskPtr m_task;
  };


  updateNumberOfThreads();
  m_pool->start(new Runnable(*this, task));
}  // WorkerThreadPool::submitTask

void WorkerThreadPool::customEvent(QEvent* event) {
  if (auto* evt = dynamic_cast<TaskResultEvent*>(event)) {
    emit taskResult(evt->task(), evt->result());
  }
}

void WorkerThreadPool::updateNumberOfThreads() {
  int maxThreads = QThread::idealThreadCount();
  // Restricting num of processors for 32-bit due to
  // address space constraints.
  if (sizeof(void*) <= 4) {
    maxThreads = std::min(maxThreads, 2);
  }

  int numThreads = m_settings.value("settings/batch_processing_threads", maxThreads).toInt();
  numThreads = std::min(numThreads, maxThreads);
  m_pool->setMaxThreadCount(numThreads);
}
