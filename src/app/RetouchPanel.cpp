// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "RetouchPanel.h"

#include <core/IconProvider.h>

#include <QAction>
#include <QActionGroup>
#include <QColorDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include "ScopedIncDec.h"

using namespace retouch;

namespace {
const int MIN_BRUSH = 2;
const int MAX_BRUSH = 600;

QIcon swatchIcon(const QColor& color, const bool automatic) {
  QPixmap pixmap(32, 32);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(QPen(QColor(128, 128, 128), 1));
  if (automatic) {
    // Half paper, half question: the colour is worked out per change.
    painter.fillRect(QRect(2, 2, 28, 28), QColor(236, 228, 210));
    painter.fillRect(QRect(16, 2, 14, 28), QColor(250, 250, 250));
    painter.setPen(QPen(QColor(90, 90, 90), 1));
    painter.drawText(QRect(2, 2, 28, 28), Qt::AlignCenter, QStringLiteral("A"));
    painter.setPen(QPen(QColor(128, 128, 128), 1));
  } else {
    painter.fillRect(QRect(2, 2, 28, 28), color);
  }
  painter.drawRect(QRect(2, 2, 27, 27));
  return QIcon(pixmap);
}

QLabel* sectionLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  QFont font(label->font());
  font.setBold(true);
  label->setFont(font);
  return label;
}

QSpinBox* spinBox(const int min, const int max, const QString& suffix, QWidget* parent) {
  auto* spin = new QSpinBox(parent);
  spin->setRange(min, max);
  spin->setSuffix(suffix);
  spin->setKeyboardTracking(false);
  return spin;
}
}  // namespace

RetouchPanel::RetouchPanel(QWidget* parent)
    : QFrame(parent),
      m_tool(Tool::RECTANGLE),
      m_fillMode(FillMode::WHITE),
      m_pickedColor(Qt::white),
      m_effectiveColor(Qt::white),
      m_expanded(true),
      m_available(false),
      m_sessionActive(false),
      m_editing(false),
      m_ignoreChanges(0) {
  setObjectName(QStringLiteral("retouchPanel"));
  setFrameShape(QFrame::StyledPanel);
  setFrameShadow(QFrame::Sunken);

  createActions();
  m_faces = new QStackedLayout(this);
  m_faces->setContentsMargins(0, 0, 0, 0);
  m_expandedFace = buildExpandedFace();
  m_collapsedFace = buildCollapsedFace();
  m_faces->addWidget(m_expandedFace);
  m_faces->addWidget(m_collapsedFace);

  restoreSettings();
  setAvailable(false);
}

RetouchPanel::~RetouchPanel() = default;

void RetouchPanel::createActions() {
  const auto& icons = IconProvider::getInstance();

  const auto makeTool = [this, &icons](const QString& icon, const QString& text, const QString& shortcut,
                                       const QString& tip, const Tool tool) {
    auto* action = new QAction(icons.getIcon(icon), text, this);
    action->setCheckable(true);
    action->setShortcut(QKeySequence(shortcut));
    action->setToolTip(QStringLiteral("%1 (%2)\n%3").arg(text, shortcut, tip));
    connect(action, &QAction::triggered, this, [this, tool]() { toolTriggered(tool); });
    addAction(action);
    return action;
  };
  m_rectAction = makeTool(QStringLiteral("retouch-rect"), tr("Rectangle"), QStringLiteral("R"),
                          tr("Drag a rectangle over the page to fill it."), Tool::RECTANGLE);
  m_brushAction = makeTool(QStringLiteral("retouch-brush"), tr("Brush"), QStringLiteral("B"),
                           tr("Paint over the page freehand."), Tool::BRUSH);
  m_selectAction = makeTool(QStringLiteral("retouch-select"), tr("Select object"), QStringLiteral("S"),
                            tr("Click a stamp or a mark to select all of it, then fill it."), Tool::SELECT);
  m_pickAction
      = makeTool(QStringLiteral("retouch-picker"), tr("Pick colour"), QStringLiteral("I"),
                 tr("Click the paper to fill with its colour. Alt+click does the same with any tool."), Tool::PICK);
  m_toolGroup = new QActionGroup(this);
  for (QAction* action : {m_rectAction, m_brushAction, m_selectAction, m_pickAction}) {
    m_toolGroup->addAction(action);
  }

  const auto makeFill = [this](const QString& text, const QString& tip, const FillMode mode) {
    auto* action = new QAction(text, this);
    action->setCheckable(true);
    action->setToolTip(tip);
    connect(action, &QAction::triggered, this, [this, mode]() { fillModeTriggered(mode); });
    return action;
  };
  m_whiteAction = makeFill(tr("White"), tr("Fill with white."), FillMode::WHITE);
  m_pickedAction = makeFill(tr("Picked"), tr("Fill with the colour picked from the page."), FillMode::PICKED);
  m_autoAction = makeFill(tr("Paper"),
                          tr("Fill each change with the colour of the paper just around it, so that the patch "
                             "blends in."),
                          FillMode::AUTO);
  m_fillGroup = new QActionGroup(this);
  for (QAction* action : {m_whiteAction, m_pickedAction, m_autoAction}) {
    m_fillGroup->addAction(action);
  }

  const auto makeAction = [this, &icons](const QString& icon, const QString& text, const QString& tip) {
    auto* action = new QAction(icon.isEmpty() ? QIcon() : icons.getIcon(icon), text, this);
    action->setToolTip(tip);
    addAction(action);
    return action;
  };
  m_undoAction = makeAction(QStringLiteral("undo"), tr("Undo"), tr("Undo the last change (Ctrl+Z)."));
  m_undoAction->setShortcut(QKeySequence::Undo);
  m_redoAction = makeAction(QStringLiteral("retouch-redo"), tr("Redo"), tr("Redo the change undone (Ctrl+Y)."));
  m_redoAction->setShortcuts({QKeySequence(Qt::CTRL + Qt::Key_Y), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z)});
  m_fillSelectionAction = makeAction(QStringLiteral("retouch-fill"), tr("Fill selection"),
                                     tr("Fill the selected object with the fill colour (Delete)."));
  m_fillSelectionAction->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
  m_clearSelectionAction
      = makeAction(QStringLiteral("retouch-close"), tr("Deselect"), tr("Clear the selection (Esc)."));
  m_clearSelectionAction->setShortcut(QKeySequence(Qt::Key_Escape));
  m_saveAction = makeAction(QStringLiteral("retouch-save"), tr("Save changes"),
                            tr("Write the changes into the page's input image - painted on the output, they are "
                               "carried into it - keeping a copy of the original. The page is then processed again "
                               "from Select Content on."));
  m_closeAction = makeAction(QStringLiteral("retouch-close"), tr("Close"),
                             tr("Stop retouching this page. Unsaved changes are asked about first."));
  m_restoreAction = makeAction(QStringLiteral("retouch-restore"), tr("Restore original..."),
                               tr("Put back the page's input image as it was before it was first retouched."));
  m_smallerBrushAction = makeAction(QString(), tr("Smaller brush"), QString());
  m_smallerBrushAction->setShortcut(QKeySequence(Qt::Key_BracketLeft));
  m_largerBrushAction = makeAction(QString(), tr("Larger brush"), QString());
  m_largerBrushAction->setShortcut(QKeySequence(Qt::Key_BracketRight));
  m_collapseAction
      = makeAction(QStringLiteral("panel-collapse"), tr("Collapse"), tr("Shrink the panel to a strip of icons."));
  m_expandAction = makeAction(QStringLiteral("panel-expand"), tr("Expand"), tr("Show the retouching options."));

  connect(m_undoAction, &QAction::triggered, this, &RetouchPanel::undoRequested);
  connect(m_redoAction, &QAction::triggered, this, &RetouchPanel::redoRequested);
  connect(m_fillSelectionAction, &QAction::triggered, this, &RetouchPanel::fillSelectionRequested);
  connect(m_clearSelectionAction, &QAction::triggered, this, &RetouchPanel::clearSelectionRequested);
  connect(m_saveAction, &QAction::triggered, this, &RetouchPanel::saveRequested);
  connect(m_closeAction, &QAction::triggered, this, &RetouchPanel::closeRequested);
  connect(m_restoreAction, &QAction::triggered, this, &RetouchPanel::restoreRequested);
  connect(m_smallerBrushAction, &QAction::triggered, this,
          [this]() { setBrushDiameter(std::max(MIN_BRUSH, int(brushDiameter() / 1.25))); });
  connect(m_largerBrushAction, &QAction::triggered, this, [this]() {
    setBrushDiameter(std::min(MAX_BRUSH, std::max(brushDiameter() + 1, int(brushDiameter() * 1.25))));
  });
  connect(m_collapseAction, &QAction::triggered, this, [this]() { setExpanded(false); });
  connect(m_expandAction, &QAction::triggered, this, [this]() { setExpanded(true); });
}

QToolButton* RetouchPanel::toolButton(QAction* action, const bool withText, QWidget* parent) {
  auto* button = new QToolButton(parent);
  button->setDefaultAction(action);
  button->setIconSize(QSize(18, 18));
  if (withText) {
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  } else {
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
  }
  return button;
}

QWidget* RetouchPanel::buildExpandedFace() {
  auto* face = new QWidget(this);
  auto* layout = new QVBoxLayout(face);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);

  auto* header = new QHBoxLayout();
  header->addWidget(sectionLabel(tr("Retouch"), face), 1);
  header->addWidget(toolButton(m_collapseAction, false, face));
  layout->addLayout(header);

  m_statusLabel = new QLabel(face);
  m_statusLabel->setWordWrap(true);
  m_statusLabel->setTextFormat(Qt::PlainText);
  layout->addWidget(m_statusLabel);

  for (QAction* action : {m_rectAction, m_brushAction, m_selectAction, m_pickAction}) {
    layout->addWidget(toolButton(action, true, face));
  }

  layout->addSpacing(4);
  layout->addWidget(sectionLabel(tr("Fill colour"), face));
  auto* fillRow = new QHBoxLayout();
  fillRow->setSpacing(3);
  m_swatch = new QToolButton(face);
  m_swatch->setIconSize(QSize(22, 22));
  m_swatch->setAutoRaise(true);
  m_swatch->setToolTip(tr("Choose the fill colour."));
  connect(m_swatch, &QToolButton::clicked, this, &RetouchPanel::chooseColor);
  fillRow->addWidget(m_swatch);
  for (QAction* action : {m_whiteAction, m_pickedAction, m_autoAction}) {
    auto* button = new QToolButton(face);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    fillRow->addWidget(button);
  }
  layout->addLayout(fillRow);
  m_colorNote = new QLabel(face);
  m_colorNote->setWordWrap(true);
  m_colorNote->setVisible(false);
  layout->addWidget(m_colorNote);

  // Brush options.
  m_brushOptions = new QWidget(face);
  auto* brushLayout = new QGridLayout(m_brushOptions);
  brushLayout->setContentsMargins(0, 4, 0, 0);
  brushLayout->addWidget(sectionLabel(tr("Brush size"), m_brushOptions), 0, 0, 1, 2);
  m_brushSlider = new QSlider(Qt::Horizontal, m_brushOptions);
  m_brushSlider->setRange(MIN_BRUSH, MAX_BRUSH);
  m_brushSpin = spinBox(MIN_BRUSH, MAX_BRUSH, tr(" px"), m_brushOptions);
  m_brushSpin->setToolTip(tr("The brush diameter in pixels of the scan. [ and ] change it."));
  brushLayout->addWidget(m_brushSlider, 1, 0);
  brushLayout->addWidget(m_brushSpin, 1, 1);
  connect(m_brushSlider, &QSlider::valueChanged, this, [this](const int value) { setBrushDiameter(value); });
  connect(m_brushSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](const int value) { setBrushDiameter(value); });
  layout->addWidget(m_brushOptions);

  // Selection options.
  m_selectOptions = new QWidget(face);
  auto* selectLayout = new QGridLayout(m_selectOptions);
  selectLayout->setContentsMargins(0, 4, 0, 0);
  selectLayout->addWidget(sectionLabel(tr("Selection"), m_selectOptions), 0, 0, 1, 2);
  selectLayout->addWidget(new QLabel(tr("Tolerance"), m_selectOptions), 1, 0);
  m_toleranceSpin = spinBox(5, 250, QString(), m_selectOptions);
  m_toleranceSpin->setToolTip(
      tr("How different from the paper a pixel must be to count as ink. Lower it for "
         "faint stamps; raise it if paper stains get selected."));
  selectLayout->addWidget(m_toleranceSpin, 1, 1);
  selectLayout->addWidget(new QLabel(tr("Grouping"), m_selectOptions), 2, 0);
  m_groupingSpin = spinBox(0, 400, tr(" px"), m_selectOptions);
  m_groupingSpin->setToolTip(
      tr("Ink this close to the clicked object is selected with it: the letters of a stamp "
         "and its frame. Lower it if nearby text gets selected too."));
  selectLayout->addWidget(m_groupingSpin, 2, 1);
  auto* selectionButtons = new QHBoxLayout();
  selectionButtons->addWidget(toolButton(m_fillSelectionAction, true, m_selectOptions));
  selectionButtons->addWidget(toolButton(m_clearSelectionAction, true, m_selectOptions));
  selectLayout->addLayout(selectionButtons, 3, 0, 1, 2);
  selectLayout->setColumnStretch(0, 1);
  const auto paramsChanged = [this]() {
    if (!m_ignoreChanges) {
      saveSettings();
      emit selectionParamsChanged();
    }
  };
  connect(m_toleranceSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, paramsChanged);
  connect(m_groupingSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, paramsChanged);
  layout->addWidget(m_selectOptions);

  layout->addSpacing(4);
  auto* history = new QHBoxLayout();
  history->addWidget(toolButton(m_undoAction, false, face));
  history->addWidget(toolButton(m_redoAction, false, face));
  m_editsLabel = new QLabel(face);
  history->addWidget(m_editsLabel, 1);
  layout->addLayout(history);

  auto* save = toolButton(m_saveAction, true, face);
  QFont saveFont(save->font());
  saveFont.setBold(true);
  save->setFont(saveFont);
  layout->addWidget(save);
  layout->addWidget(toolButton(m_closeAction, true, face));
  layout->addWidget(toolButton(m_restoreAction, true, face));

  layout->addStretch(1);
  auto* help = new QLabel(tr("Changes go into the page's input image - painted on the output, they are carried into "
                             "it through the crop and the skew - and the page is processed again from Select "
                             "Content on. Middle button or Space+drag moves the image; the wheel zooms."),
                          face);
  help->setWordWrap(true);
  help->setEnabled(false);
  layout->addWidget(help);

  face->setMinimumWidth(190);
  face->setMaximumWidth(230);
  return face;
}

QWidget* RetouchPanel::buildCollapsedFace() {
  auto* face = new QWidget(this);
  auto* layout = new QVBoxLayout(face);
  layout->setContentsMargins(2, 4, 2, 4);
  layout->setSpacing(2);
  layout->addWidget(toolButton(m_expandAction, false, face));
  layout->addSpacing(6);
  for (QAction* action : {m_rectAction, m_brushAction, m_selectAction, m_pickAction}) {
    layout->addWidget(toolButton(action, false, face));
  }
  m_collapsedSwatch = new QToolButton(face);
  m_collapsedSwatch->setIconSize(QSize(18, 18));
  m_collapsedSwatch->setAutoRaise(true);
  m_collapsedSwatch->setToolTip(tr("Choose the fill colour."));
  connect(m_collapsedSwatch, &QToolButton::clicked, this, &RetouchPanel::chooseColor);
  layout->addWidget(m_collapsedSwatch);
  layout->addSpacing(6);
  layout->addWidget(toolButton(m_undoAction, false, face));
  layout->addWidget(toolButton(m_redoAction, false, face));
  layout->addSpacing(6);
  layout->addWidget(toolButton(m_saveAction, false, face));
  layout->addWidget(toolButton(m_closeAction, false, face));
  layout->addStretch(1);
  face->setFixedWidth(30);
  return face;
}

void RetouchPanel::setTool(const Tool tool) {
  m_tool = tool;
  switch (tool) {
    case Tool::RECTANGLE:
      m_rectAction->setChecked(true);
      break;
    case Tool::BRUSH:
      m_brushAction->setChecked(true);
      break;
    case Tool::SELECT:
      m_selectAction->setChecked(true);
      break;
    case Tool::PICK:
      m_pickAction->setChecked(true);
      break;
  }
  updateToolOptions();
  saveSettings();
}

void RetouchPanel::toolTriggered(const Tool tool) {
  setTool(tool);
  emit toolActivated(tool);
}

void RetouchPanel::fillModeTriggered(const FillMode mode) {
  m_fillMode = mode;
  updateSwatches();
  saveSettings();
  emit fillChanged();
}

void RetouchPanel::setPickedColor(const QColor& color) {
  if (!color.isValid()) {
    return;
  }
  m_pickedColor = color;
  m_fillMode = FillMode::PICKED;
  m_pickedAction->setChecked(true);
  updateSwatches();
  saveSettings();
  emit fillChanged();
}

void RetouchPanel::chooseColor() {
  const QColor color(QColorDialog::getColor(m_pickedColor, window(), tr("Fill Colour")));
  if (color.isValid()) {
    setPickedColor(color);
  }
}

int RetouchPanel::brushDiameter() const {
  return m_brushSpin->value();
}

void RetouchPanel::setBrushDiameter(const int pixels) {
  const int value = qBound(MIN_BRUSH, pixels, MAX_BRUSH);
  const bool changed = (value != m_brushSpin->value()) || (value != m_brushSlider->value());
  {
    const ScopedIncDec<int> guard(m_ignoreChanges);
    m_brushSpin->setValue(value);
    m_brushSlider->setValue(value);
  }
  if (changed && !m_ignoreChanges) {
    saveSettings();
    emit brushDiameterChanged(value);
  }
}

ObjectSelectionParams RetouchPanel::selectionParams() const {
  ObjectSelectionParams params;
  params.tolerance = m_toleranceSpin->value();
  params.grouping = m_groupingSpin->value();
  return params;
}

void RetouchPanel::setExpanded(const bool expanded) {
  m_expanded = expanded;
  m_faces->setCurrentWidget(expanded ? m_expandedFace : m_collapsedFace);
  // A stacked layout takes the width of its widest page unless told otherwise.
  setFixedWidth(expanded ? m_expandedFace->sizeHint().width() + 2 * frameWidth()
                         : m_collapsedFace->width() + 2 * frameWidth());
  saveSettings();
}

void RetouchPanel::setAvailable(const bool available, const QString& reason) {
  m_available = available;
  m_toolGroup->setEnabled(available);
  m_smallerBrushAction->setEnabled(available);
  m_largerBrushAction->setEnabled(available);
  if (!available) {
    m_restoreAction->setEnabled(false);
    m_statusLabel->setText(reason.isEmpty() ? tr("Open a project to retouch its pages.") : reason);
  } else if (!m_sessionActive) {
    m_statusLabel->setText(tr("Pick a tool to retouch this page."));
  }
}

void RetouchPanel::setSessionState(const bool loading,
                                   const bool editing,
                                   const QString& fileName,
                                   const QString& note) {
  m_sessionActive = loading || editing;
  m_editing = editing;
  m_closeAction->setEnabled(m_sessionActive);
  if (loading) {
    m_statusLabel->setText(tr("Loading %1...").arg(fileName));
  } else if (editing) {
    QString text(tr("Editing %1").arg(fileName));
    if (!note.isEmpty()) {
      text += QLatin1String("\n\n") + note;
    }
    m_statusLabel->setText(text);
  } else if (m_available) {
    m_statusLabel->setText(tr("Pick a tool to retouch this page."));
  }
  if (!editing) {
    setEditState(0, false, false);
    setHasSelection(false);
  }
}

void RetouchPanel::setEditState(const int edits, const bool canUndo, const bool canRedo) {
  m_undoAction->setEnabled(m_editing && canUndo);
  m_redoAction->setEnabled(m_editing && canRedo);
  m_saveAction->setEnabled(m_editing && (edits > 0));
  m_editsLabel->setText(m_editing ? tr("%n change(s)", nullptr, edits) : QString());
}

void RetouchPanel::setHasSelection(const bool hasSelection) {
  m_fillSelectionAction->setEnabled(m_editing && hasSelection);
  m_clearSelectionAction->setEnabled(m_editing && hasSelection);
}

void RetouchPanel::setRestoreAvailable(const bool available) {
  m_restoreAction->setEnabled(m_available && available);
}

void RetouchPanel::setEffectiveColor(const QColor& color, const QString& note) {
  m_effectiveColor = color;
  m_colorNote->setText(note);
  m_colorNote->setVisible(!note.isEmpty() && m_fillMode != FillMode::AUTO);
  updateSwatches();
}

void RetouchPanel::updateToolOptions() {
  m_brushOptions->setVisible(m_tool == Tool::BRUSH);
  m_selectOptions->setVisible(m_tool == Tool::SELECT);
}

void RetouchPanel::updateSwatches() {
  const bool automatic = (m_fillMode == FillMode::AUTO);
  QColor shown(Qt::white);
  if (m_fillMode == FillMode::PICKED) {
    shown = m_pickedColor;
  }
  if (m_effectiveColor.isValid() && !automatic) {
    shown = m_effectiveColor;
  }
  const QIcon icon(swatchIcon(shown, automatic));
  m_swatch->setIcon(icon);
  m_collapsedSwatch->setIcon(icon);
  const QString tip(automatic ? tr("Fill colour: the paper around each change")
                              : tr("Fill colour: %1").arg(shown.name()));
  m_collapsedSwatch->setToolTip(tip + tr("\nClick to choose another."));
  m_swatch->setToolTip(tip + tr("\nClick to choose another."));
}

void RetouchPanel::saveSettings() const {
  if (m_ignoreChanges) {
    return;
  }
  QSettings settings;
  settings.setValue(QStringLiteral("retouch/expanded"), m_expanded);
  settings.setValue(QStringLiteral("retouch/tool"), static_cast<int>(m_tool));
  settings.setValue(QStringLiteral("retouch/fillMode"), static_cast<int>(m_fillMode));
  settings.setValue(QStringLiteral("retouch/pickedColor"), m_pickedColor.name());
  settings.setValue(QStringLiteral("retouch/brushDiameter"), m_brushSpin->value());
  settings.setValue(QStringLiteral("retouch/tolerance"), m_toleranceSpin->value());
  settings.setValue(QStringLiteral("retouch/grouping"), m_groupingSpin->value());
}

void RetouchPanel::restoreSettings() {
  const ScopedIncDec<int> guard(m_ignoreChanges);
  const QSettings settings;
  const ObjectSelectionParams defaults;
  m_brushSpin->setValue(settings.value(QStringLiteral("retouch/brushDiameter"), 40).toInt());
  m_brushSlider->setValue(m_brushSpin->value());
  m_toleranceSpin->setValue(settings.value(QStringLiteral("retouch/tolerance"), defaults.tolerance).toInt());
  m_groupingSpin->setValue(settings.value(QStringLiteral("retouch/grouping"), 40).toInt());
  const QColor picked(settings.value(QStringLiteral("retouch/pickedColor"), QStringLiteral("#ffffff")).toString());
  m_pickedColor = picked.isValid() ? picked : QColor(Qt::white);

  const int fillMode = settings.value(QStringLiteral("retouch/fillMode"), 0).toInt();
  m_fillMode = (fillMode == static_cast<int>(FillMode::PICKED))
                   ? FillMode::PICKED
                   : ((fillMode == static_cast<int>(FillMode::AUTO)) ? FillMode::AUTO : FillMode::WHITE);
  (m_fillMode == FillMode::PICKED ? m_pickedAction : (m_fillMode == FillMode::AUTO ? m_autoAction : m_whiteAction))
      ->setChecked(true);

  // Picking is a step, not a tool to come back to.
  const int tool = settings.value(QStringLiteral("retouch/tool"), 0).toInt();
  setTool((tool == static_cast<int>(Tool::BRUSH))
              ? Tool::BRUSH
              : ((tool == static_cast<int>(Tool::SELECT)) ? Tool::SELECT : Tool::RECTANGLE));
  updateSwatches();
  setExpanded(settings.value(QStringLiteral("retouch/expanded"), true).toBool());
}
