// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_APPUPDATER_H_
#define SCANTAILOR_APP_APPUPDATER_H_

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

#include "AppUpdate.h"

class QLabel;
class QMainWindow;
class QThread;

/**
 * \brief Keeps the application up to date from the installer on the office share.
 *
 * The installer there keeps its file name from one release to the next, so a
 * newer one is told by the product version it carries. When one appears the
 * operator is told - in Romanian, as the operators asked - and the application
 * closes, runs it and starts again: straight away when no project is open,
 * otherwise once the project is closed, or when the application is.
 *
 * The installer runs from a local copy, started by a small script that waits
 * for this process to end. It removes the old version before installing the
 * new one, so it must not start while anything still runs from the
 * installation folder: that would leave the installation half removed.
 */
class AppUpdater : public QObject {
  Q_OBJECT
 public:
  /**
   * \param window The main window: what closes for the update, and the parent of its messages.
   * \param busy Whether closing now would interrupt the operator: a project open, a batch
   *        running, the window already closing.
   */
  AppUpdater(QMainWindow* window, std::function<bool()> busy);

  ~AppUpdater() override;

  /** \brief Whether this copy keeps itself up to date: an installed copy does, unless its settings say not. */
  static bool isEnabled();

  /** \brief Reports how the last update went, and starts looking: shortly, and then every hour. */
  void start();

  /** \brief The start page is up again: an update found meanwhile can be installed now. */
  void becameIdle();

  /**
   * \brief The application is closing for good, its project already closed.
   *
   * Runs the update the operator asked for, or offers one found while they worked.
   */
  void applicationClosing();

  /** \brief The operator cancelled closing the application for the update. */
  void closeCancelled();

 private:
  void check();

  void checkFinished(const core::AppUpdate::Installer& installer);

  /** \brief Whether installing the available version has already failed once. */
  bool failedBefore() const;

  /** \brief Asks to update now; \p countdown makes it go ahead if nobody answers. */
  void offer(bool countdown);

  /** \brief "Update" or "not now"; \p countdown seconds without an answer count as "update". */
  bool ask(const QString& text, const QString& declineText, int countdown);

  /** \brief Closes the application to install, once the operator has agreed. */
  void requestInstall();

  /** \brief Copies the installer to the local disk, and checks the copy. */
  bool prepareInstaller(QString* error);

  /** \brief Starts the script that installs once this process has ended. */
  bool launchInstaller(bool restart);

  void reportPreviousAttempt();

  void showNotice();

  QMainWindow* m_window;
  std::function<bool()> m_busy;
  QTimer m_timer;
  QPointer<QThread> m_checkThread;
  QPointer<QLabel> m_notice;
  core::AppUpdate::Installer m_available;
  QString m_localInstaller;
  bool m_installRequested = false;
  bool m_declined = false;
  bool m_asking = false;
  bool m_closing = false;
};

#endif  // SCANTAILOR_APP_APPUPDATER_H_
