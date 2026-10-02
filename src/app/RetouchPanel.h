// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_RETOUCHPANEL_H_
#define SCANTAILOR_APP_RETOUCHPANEL_H_

#include <Selection.h>

#include <QColor>
#include <QFrame>

#include "RetouchTypes.h"

class QAction;
class QActionGroup;
class QLabel;
class QSlider;
class QSpinBox;
class QStackedLayout;
class QToolButton;
class QWidget;

/**
 * \brief The retouching tools, on the left of the page.
 *
 * Two faces over the same actions: expanded, with every option, or collapsed to
 * a strip of icons one button wide, so that the tools take no room from the
 * page while they are not needed and stay one click away when they are. The
 * choice, like the tool and its settings, is remembered between sessions.
 *
 * The panel only presents and collects: the session it controls lives in
 * RetouchController.
 */
class RetouchPanel : public QFrame {
  Q_OBJECT
 public:
  explicit RetouchPanel(QWidget* parent = nullptr);

  ~RetouchPanel() override;

  retouch::Tool tool() const { return m_tool; }

  /** \brief Checks \p tool without announcing it, for when the controller changes it. */
  void setTool(retouch::Tool tool);

  retouch::FillMode fillMode() const { return m_fillMode; }

  const QColor& pickedColor() const { return m_pickedColor; }

  /** \brief Takes \p color as the picked colour and switches to it. */
  void setPickedColor(const QColor& color);

  int brushDiameter() const;

  void setBrushDiameter(int pixels);

  retouch::ObjectSelectionParams selectionParams() const;

  bool isExpanded() const { return m_expanded; }

  void setExpanded(bool expanded);

  /**
   * \brief Whether the tools can be used at all: a project is open, no batch is
   *        running, the Output stage is on screen. \p reason says why not.
   */
  void setAvailable(bool available, const QString& reason = QString());

  /**
   * \brief Shows what is going on: nothing, or \p fileName being loaded or edited.
   *
   * \param note What else the operator should know about the session, if anything.
   */
  void setSessionState(bool loading, bool editing, const QString& fileName, const QString& note = QString());

  void setEditState(int edits, bool canUndo, bool canRedo);

  void setHasSelection(bool hasSelection);

  void setRestoreAvailable(bool available);

  /** \brief The fill colour as the current image can hold it, with a note when it differs. */
  void setEffectiveColor(const QColor& color, const QString& note);

 signals:
  void toolActivated(retouch::Tool tool);

  void fillChanged();

  void brushDiameterChanged(int pixels);

  void selectionParamsChanged();

  void undoRequested();

  void redoRequested();

  void fillSelectionRequested();

  void clearSelectionRequested();

  void saveRequested();

  void closeRequested();

  void restoreRequested();

 private:
  void createActions();

  QWidget* buildExpandedFace();

  QWidget* buildCollapsedFace();

  QToolButton* toolButton(QAction* action, bool withText, QWidget* parent);

  void toolTriggered(retouch::Tool tool);

  void fillModeTriggered(retouch::FillMode mode);

  void chooseColor();

  void updateToolOptions();

  void updateSwatches();

  void saveSettings() const;

  void restoreSettings();

  QStackedLayout* m_faces;
  QWidget* m_expandedFace;
  QWidget* m_collapsedFace;

  QAction* m_rectAction;
  QAction* m_brushAction;
  QAction* m_selectAction;
  QAction* m_pickAction;
  QActionGroup* m_toolGroup;
  QAction* m_whiteAction;
  QAction* m_pickedAction;
  QAction* m_autoAction;
  QActionGroup* m_fillGroup;
  QAction* m_undoAction;
  QAction* m_redoAction;
  QAction* m_fillSelectionAction;
  QAction* m_clearSelectionAction;
  QAction* m_saveAction;
  QAction* m_closeAction;
  QAction* m_restoreAction;
  QAction* m_smallerBrushAction;
  QAction* m_largerBrushAction;
  QAction* m_collapseAction;
  QAction* m_expandAction;

  QLabel* m_statusLabel;
  QLabel* m_editsLabel;
  QLabel* m_colorNote;
  QToolButton* m_swatch;
  QToolButton* m_collapsedSwatch;
  QWidget* m_brushOptions;
  QWidget* m_selectOptions;
  QSlider* m_brushSlider;
  QSpinBox* m_brushSpin;
  QSpinBox* m_toleranceSpin;
  QSpinBox* m_groupingSpin;

  retouch::Tool m_tool;
  retouch::FillMode m_fillMode;
  QColor m_pickedColor;
  QColor m_effectiveColor;
  bool m_expanded;
  bool m_available;
  bool m_sessionActive;
  bool m_editing;
  int m_ignoreChanges;
};


#endif  // SCANTAILOR_APP_RETOUCHPANEL_H_
