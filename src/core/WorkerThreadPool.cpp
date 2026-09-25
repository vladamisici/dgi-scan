// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "WorkerThreadPool.h"

#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QThreadPool>
#include <cstring>
#include <utility>

#include "Diagnostics.h"
#include "OutOfMemoryHandler.h"

class WorkerThreadPool::TaskResultEvent : public QEvent {
 public:
  TaskResultEvent(BackgroundTaskPtr task, FilterResultPtr result)
      : QEvent(User),
        m_task(std::move(task)),
        m_result(std::move(result)),
        m_postedNs(core::diag::enabled() ? core::diag::nowNs() : 0) {}

  const BackgroundTaskPtr& task() const { return m_task; }

  const FilterResultPtr& result() const { return m_result; }

  /** When the worker posted this result, or 0 if diagnostics were off. */
  qint64 postedNs() const { return m_postedNs; }

 private:
  BackgroundTaskPtr m_task;
  FilterResultPtr m_result;
  qint64 m_postedNs;
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
  DIAG_SCOPE(diagScope, "pool.shutdown");
  m_pool->clear();
  if (!m_pool->waitForDone(15000)) {
    qWarning() << "Worker thread pool did not finish within 15 seconds; shutting down anyway";
  }
}

bool WorkerThreadPool::isIdle() const {
  return m_pool->activeThreadCount() == 0;
}

bool WorkerThreadPool::hasSpareCapacity() const {
  return m_pool->activeThreadCount() < m_pool->maxThreadCount();
}

void WorkerThreadPool::submitTask(const BackgroundTaskPtr& task) {
  class Runnable : public QRunnable {
   public:
    Runnable(WorkerThreadPool& owner, BackgroundTaskPtr task)
        : m_owner(owner), m_task(std::move(task)), m_submittedNs(core::diag::nowNs()) {
      setAutoDelete(true);
    }

    void run() override {
      // Set on every run, not once per thread: QThreadPool creates and retires
      // its threads on its own schedule, and the call is only a pointer store.
      core::diag::setThreadRole("worker");
      applyPriority();
      // Declared before the scope so the task.run record still carries the task id.
      const TaskMarker taskMarker;
      DIAG_SCOPE(diagScope, "task.run");
      const qint64 queuedNs = core::diag::nowNs() - m_submittedNs;

      if (m_task->isCancelled()) {
        describeRun(diagScope, queuedNs, "cancelled");
        return;
      }

      // Only a pointer to a literal is stored in the handlers below: after a
      // bad_alloc, building the attribute there could fail all over again.
      const char* outcome = "ok";
      try {
        const FilterResultPtr result((*m_task)());
        if (result) {
          QCoreApplication::postEvent(&m_owner, new TaskResultEvent(m_task, result));
        } else {
          outcome = "null";
        }
      } catch (const std::bad_alloc&) {
        outcome = "bad_alloc";
        OutOfMemoryHandler::instance().handleOutOfMemorySituation();
        reportFailure();
      } catch (const std::exception& e) {
        outcome = "exception";
        // Until these handlers existed, anything other than bad_alloc escaped
        // QRunnable::run() and reached std::terminate, which kills the process
        // outright - no dialog, no chance to save. Dropping one page's result
        // costs that page a reprocess; letting the exception through cost the
        // operator their entire session.
        qCritical() << "Background task failed:" << e.what();
        reportFailure();
      } catch (...) {
        outcome = "unknown";
        qCritical() << "Background task failed with an unknown exception";
        reportFailure();
      }
      describeRun(diagScope, queuedNs, outcome);
    }

   private:
    /**
     * Per run, because QThreadPool reuses and retires its threads on its own
     * schedule. With every logical CPU busy on page processing - and operators
     * run three instances at once - workers at the GUI thread's own priority
     * compete with it for the CPU, and the window stops responding although it
     * has nothing to do. Measured with the diagnostics suite before being made a
     * default; until then it is opt-in.
     */
    void applyPriority() {
      const QThread::Priority wanted
          = m_owner.m_lowPriority.load(std::memory_order_relaxed) ? QThread::LowPriority : QThread::NormalPriority;
      QThread* const thread = QThread::currentThread();
      if (thread->priority() != wanted) {
        thread->setPriority(wanted);
      }
    }

    /** Tags every diagnostics record this thread writes during the task with the task's id. */
    struct TaskMarker {
      TaskMarker() { core::diag::beginTask(); }

      ~TaskMarker() { core::diag::endTask(); }

      TaskMarker(const TaskMarker&) = delete;

      TaskMarker& operator=(const TaskMarker&) = delete;
    };


    /**
     * Must not throw: it runs outside the handlers above, and an exception
     * escaping run() is the very process termination they exist to prevent.
     * Building the attributes allocates, which can fail right after a bad_alloc.
     */
    void describeRun(core::diag::Scope& scope, const qint64 queuedNs, const char* outcome) const noexcept {
      if (!core::diag::enabled()) {
        return;
      }
      // A failure reaches the log however quickly it happened: at the basic level
      // only slow tasks are written, and a corrupt page that throws in 200 ms
      // would otherwise leave no trace at all.
      if ((std::strcmp(outcome, "ok") != 0) && (std::strcmp(outcome, "cancelled") != 0)) {
        scope.forceRecord();
      }
      try {
        scope.attr(core::diag::Attr("type", (m_task->type() == BackgroundTask::BATCH) ? "batch" : "interactive"));
        scope.attr(core::diag::Attr("queue_ms", queuedNs / 1e6));
        scope.attr(core::diag::Attr("outcome", outcome));
      } catch (...) {
        // The record is still written, just without these attributes.
      }
    }

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
    const qint64 m_submittedNs;
  };


  updateNumberOfThreads();
  m_pool->start(new Runnable(*this, task));
}  // WorkerThreadPool::submitTask

void WorkerThreadPool::customEvent(QEvent* event) {
  if (auto* evt = dynamic_cast<TaskResultEvent*>(event)) {
    if (evt->postedNs() != 0) {
      // Time the result sat in the GUI thread's event queue. A worker can finish
      // a page promptly and the operator still wait for it, if the GUI thread is
      // busy elsewhere; this is where that wait shows up.
      static core::diag::Counter deliverCounter("task.deliver");
      deliverCounter.add(core::diag::nowNs() - evt->postedNs());
    }
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

  m_lowPriority.store(m_settings.value("settings/worker_thread_priority").toString() == QLatin1String("low"),
                      std::memory_order_relaxed);
}
