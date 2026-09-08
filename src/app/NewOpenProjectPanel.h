// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_NEWOPENPROJECTPANEL_H_
#define SCANTAILOR_APP_NEWOPENPROJECTPANEL_H_

#include <core/ProjectHistory.h>

#include <QWidget>
#include <memory>

#include "ui_NewOpenProjectPanel.h"

class QString;
class QLabel;

class NewOpenProjectPanel : public QWidget, private Ui::NewOpenProjectPanel {
  Q_OBJECT
 public:
  explicit NewOpenProjectPanel(QWidget* parent = nullptr);

 signals:

  void newProject();

  void openProject();

  void openRecentProject(const QString& projectFile);

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  /** \brief Rebuilds the history list from m_history. */
  void populateHistory();

  void addHistoryEntry(const core::ProjectHistory::Entry& entry);

  /** \brief Right-click menu for one entry: open, reveal, forget. */
  void showEntryContextMenu(const core::ProjectHistory::Entry& entry, const QPoint& globalPos);

  void removeEntry(const QString& filePath);

  void clearHistory();

  core::ProjectHistory m_history;
  std::vector<QWidget*> m_historyRows;
  QLabel* m_clearHistoryLabel = nullptr;
};


#endif  // ifndef SCANTAILOR_APP_NEWOPENPROJECTPANEL_H_
