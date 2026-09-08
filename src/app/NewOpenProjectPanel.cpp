// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "NewOpenProjectPanel.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QUrl>
#include <QVBoxLayout>

#include "ColorSchemeManager.h"
#include "Utils.h"

using namespace core;

namespace {
/** Dimmed colour for the secondary line of a history entry. */
QColor secondaryTextColor(const QPalette& palette) {
  QColor color = ColorSchemeManager::instance().getColorParam("OpenNewProjectText", palette.windowText()).color();
  color.setAlpha(150);
  return color;
}
}  // namespace

NewOpenProjectPanel::NewOpenProjectPanel(QWidget* parent) : QWidget(parent) {
  setupUi(this);

  newProjectLabel->setText(Utils::richTextForLink(newProjectLabel->text()));
  openProjectLabel->setText(Utils::richTextForLink(openProjectLabel->text()));

  m_history.read();
  populateHistory();

  connect(newProjectLabel, SIGNAL(linkActivated(const QString&)), this, SIGNAL(newProject()));
  connect(openProjectLabel, SIGNAL(linkActivated(const QString&)), this, SIGNAL(openProject()));
}

void NewOpenProjectPanel::populateHistory() {
  for (QWidget* row : m_historyRows) {
    recentProjectsGroup->layout()->removeWidget(row);
    row->deleteLater();
  }
  m_historyRows.clear();
  if (m_clearHistoryLabel) {
    recentProjectsGroup->layout()->removeWidget(m_clearHistoryLabel);
    m_clearHistoryLabel->deleteLater();
    m_clearHistoryLabel = nullptr;
  }

  if (m_history.isEmpty()) {
    recentProjectsGroup->setVisible(false);
    return;
  }
  recentProjectsGroup->setVisible(true);

  for (const ProjectHistory::Entry& entry : m_history.entries()) {
    addHistoryEntry(entry);
  }

  m_clearHistoryLabel = new QLabel(recentProjectsGroup);
  m_clearHistoryLabel->setTextFormat(Qt::RichText);
  m_clearHistoryLabel->setText(Utils::richTextForLink(tr("Clear the list"), QLatin1String("#clear")));
  QFont clearFont = m_clearHistoryLabel->font();
  clearFont.setPointSize(std::max(1, recentProjectsGroup->font().pointSize() - 6));
  m_clearHistoryLabel->setFont(clearFont);
  connect(m_clearHistoryLabel, &QLabel::linkActivated, this, [this](const QString&) { clearHistory(); });
  recentProjectsGroup->layout()->addWidget(m_clearHistoryLabel);
}

void NewOpenProjectPanel::addHistoryEntry(const ProjectHistory::Entry& entry) {
  const int baseFontSize = recentProjectsGroup->font().pointSize();
  const bool available = entry.isAvailable();

  auto* row = new QWidget(recentProjectsGroup);
  auto* rowLayout = new QVBoxLayout(row);
  rowLayout->setContentsMargins(0, 0, 0, 4);
  rowLayout->setSpacing(0);

  auto* nameLabel = new QLabel(row);
  nameLabel->setWordWrap(true);
  nameLabel->setTextFormat(Qt::RichText);
  QFont nameFont = nameLabel->font();
  nameFont.setPointSize(baseFontSize);
  nameLabel->setFont(nameFont);

  if (available) {
    nameLabel->setText(Utils::richTextForLink(entry.displayName(), entry.filePath));
    connect(nameLabel, SIGNAL(linkActivated(const QString&)), this, SIGNAL(openRecentProject(const QString&)));
  } else {
    // Kept rather than hidden: a project on a share that happens to be offline
    // is not a project the operator has finished with.
    nameLabel->setText(entry.displayName().toHtmlEscaped());
    nameLabel->setEnabled(false);
  }
  rowLayout->addWidget(nameLabel);

  QStringList details;
  if (entry.pageCount > 0) {
    details << ((entry.pageCount == 1) ? tr("1 page") : tr("%1 pages").arg(entry.pageCount));
  }
  details << entry.lastOpenedDescription();
  if (!available) {
    details << tr("not available");
  }

  // Built from its code point rather than written as a literal: MSVC reads the
  // source in the local codepage, which turns a middle dot into mojibake.
  const QString separator = QLatin1String("  ") + QChar(0x00B7) + QLatin1String("  ");

  auto* detailLabel = new QLabel(row);
  detailLabel->setTextFormat(Qt::PlainText);
  detailLabel->setText(details.join(separator));
  QFont detailFont = detailLabel->font();
  detailFont.setPointSize(std::max(1, baseFontSize - 6));
  detailLabel->setFont(detailFont);
  QPalette detailPalette = detailLabel->palette();
  detailPalette.setColor(QPalette::WindowText, secondaryTextColor(palette()));
  detailLabel->setPalette(detailPalette);
  rowLayout->addWidget(detailLabel);

  const QString tooltip = entry.outputDirectory.isEmpty()
                              ? entry.filePath
                              : tr("%1\nOutput: %2").arg(entry.filePath, entry.outputDirectory);
  row->setToolTip(tooltip);

  row->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(row, &QWidget::customContextMenuRequested, this,
          [this, entry, row](const QPoint& pos) { showEntryContextMenu(entry, row->mapToGlobal(pos)); });

  recentProjectsGroup->layout()->addWidget(row);
  m_historyRows.push_back(row);
}

void NewOpenProjectPanel::showEntryContextMenu(const ProjectHistory::Entry& entry, const QPoint& globalPos) {
  QMenu menu(this);

  QAction* const openAction = menu.addAction(tr("Open"));
  openAction->setEnabled(entry.isAvailable());

  QAction* const revealAction = menu.addAction(tr("Open Containing Folder"));
  revealAction->setEnabled(QFileInfo::exists(QFileInfo(entry.filePath).absolutePath()));

  menu.addSeparator();
  QAction* const removeAction = menu.addAction(tr("Remove from List"));

  QAction* const chosen = menu.exec(globalPos);
  if (!chosen) {
    return;
  }
  if (chosen == openAction) {
    emit openRecentProject(entry.filePath);
  } else if (chosen == revealAction) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(entry.filePath).absolutePath()));
  } else if (chosen == removeAction) {
    removeEntry(entry.filePath);
  }
}

void NewOpenProjectPanel::removeEntry(const QString& filePath) {
  m_history.remove(filePath);
  m_history.write();
  populateHistory();
}

void NewOpenProjectPanel::clearHistory() {
  m_history.clear();
  m_history.write();
  populateHistory();
}

void NewOpenProjectPanel::paintEvent(QPaintEvent*) {
  // In fact Qt doesn't draw QWidget's background, unless
  // autoFillBackground property is set, so we can safely
  // draw our borders and shadows in the margins area.

  int left = 0, top = 0, right = 0, bottom = 0;
  layout()->getContentsMargins(&left, &top, &right, &bottom);

  const QRect widgetRect(rect());
  const QRect exceptMargins(widgetRect.adjusted(left, top, -right, -bottom));

  const int border = 1;  // Solid line border width.

  QPainter painter(this);

  const QBrush borderBrush
      = ColorSchemeManager::instance().getColorParam("OpenNewProjectBorder", palette().windowText());
  painter.setPen(QPen(borderBrush, border));

  painter.drawRect(exceptMargins);
}  // NewOpenProjectPanel::paintEvent
