// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "RetouchView.h"

#include <Proximity.h>

#include <QApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <algorithm>
#include <cmath>

#include "Dpm.h"
#include "ImagePresentation.h"
#include "ImageTransformation.h"

using namespace retouch;

RetouchView::RetouchView(const QImage& source,
                         const QImage& display,
                         const QImage& downscaled,
                         const ImageTransformation& xform)
    : ImageViewBase(display, downscaled, ImagePresentation(xform.transform(), xform.resultingPreCropArea())),
      m_source(source),
      m_dragHandler(*this),
      m_zoomHandler(*this),
      m_applied(0),
      m_tool(Tool::RECTANGLE),
      m_fillMode(FillMode::WHITE),
      m_pickedColor(Qt::white),
      m_brushDiameter(40),
      m_paperRing(4),
      m_cursorInside(false),
      m_spaceDown(false) {
  // The band sampled for the paper colour: half a millimetre, which clears the
  // soft edge of the ink without reaching far into the page.
  const Dpi dpi{Dpm(display)};
  if (!dpi.isNull()) {
    m_paperRing = std::max(4, qRound(dpi.horizontal() / 50.0));
  }

  // The tools first, so that a left click paints rather than drags; dragging and
  // zooming come after, as in every other view.
  rootInteractionHandler().makeLastFollower(*this);
  rootInteractionHandler().makeLastFollower(m_dragHandler);
  rootInteractionHandler().makeLastFollower(m_zoomHandler);

  updateToolCursor();
}

RetouchView::~RetouchView() = default;

void RetouchView::setTool(const Tool tool) {
  if (m_tool == tool) {
    return;
  }
  m_tool = tool;
  m_dragStart = m_dragEnd = QPointF();
  m_stroke.clear();
  m_paintCaptor.release();
  updateToolCursor();
  update();
}

void RetouchView::setFillMode(const FillMode mode, const QColor& pickedColor) {
  m_fillMode = mode;
  m_pickedColor = pickedColor;
  update();
}

void RetouchView::setBrushDiameter(const int pixels) {
  m_brushDiameter = std::max(1, pixels);
  update();
}

void RetouchView::setSelectionParams(const ObjectSelectionParams& params) {
  m_selectionParams = params;
  // The soft edge of ink is a fixed width on paper, so more pixels at higher
  // resolutions: 2 at 300 dpi, 3 at 600.
  m_selectionParams.margin = std::max(params.margin, m_paperRing / 4);
}

std::vector<Edit> RetouchView::edits() const {
  return std::vector<Edit>(m_edits.begin(), m_edits.begin() + m_applied);
}

void RetouchView::undo() {
  if (canUndo()) {
    --m_applied;
    emit editsChanged();
    update();
  }
}

void RetouchView::redo() {
  if (canRedo()) {
    ++m_applied;
    emit editsChanged();
    update();
  }
}

void RetouchView::fillSelection() {
  if (m_selection.isNull()) {
    return;
  }
  commit(Edit::mask(m_selection.mask(), m_selection.origin(), Qt::white));
  clearSelection();
}

void RetouchView::clearSelection() {
  if (m_selection.isNull()) {
    return;
  }
  m_selection = Selection();
  m_selectionImage = QImage();
  emit selectionChanged();
  update();
}

QColor RetouchView::effectiveColor(const QColor& requested) const {
  return retouch::effectiveColor(m_source, requested);
}

QPointF RetouchView::toImage(const QPointF& widgetPos) const {
  return widgetToImage().map(widgetPos);
}

bool RetouchView::panning() const {
  return m_spaceDown;
}

void RetouchView::onProximityUpdate(const QPointF&, InteractionState& interaction) {
  // Every click on this view is a tool click, so the tool's cursor and help
  // hold over the whole of it.
  interaction.updateProximity(m_hoverCaptor, Proximity::fromDist(0.0), 1);
}

void RetouchView::onMousePressEvent(QMouseEvent* event, InteractionState& interaction) {
  if (interaction.captured() || (event->button() != Qt::LeftButton) || panning()) {
    return;
  }
  const QPointF imagePos(toImage(QPointF(event->pos()) + QPointF(0.5, 0.5)));

  if ((event->modifiers() & Qt::AltModifier) || (m_tool == Tool::PICK)) {
    pickColorAt(imagePos);
    event->accept();
    return;
  }

  switch (m_tool) {
    case Tool::RECTANGLE:
      m_dragStart = m_dragEnd = imagePos;
      interaction.capture(m_paintCaptor);
      break;
    case Tool::BRUSH:
      m_stroke.clear();
      m_stroke << imagePos;
      interaction.capture(m_paintCaptor);
      break;
    case Tool::SELECT:
      selectAt(imagePos, event->modifiers());
      break;
    case Tool::PICK:
      break;
  }
  event->accept();
  update();
}

void RetouchView::onMouseMoveEvent(QMouseEvent* event, InteractionState& interaction) {
  m_cursorPos = QPointF(event->pos()) + QPointF(0.5, 0.5);
  m_cursorInside = true;
  if (interaction.capturedBy(m_paintCaptor)) {
    const QPointF imagePos(toImage(m_cursorPos));
    if (m_tool == Tool::RECTANGLE) {
      m_dragEnd = imagePos;
    } else if ((m_tool == Tool::BRUSH) && (QLineF(m_stroke.back(), imagePos).length() >= 1.0)) {
      m_stroke << imagePos;
    }
    event->accept();
  }
  // The brush outline follows the cursor.
  update();
}

void RetouchView::onMouseReleaseEvent(QMouseEvent* event, InteractionState& interaction) {
  if (!interaction.capturedBy(m_paintCaptor) || (event->button() != Qt::LeftButton)) {
    return;
  }
  m_paintCaptor.release();

  if (m_tool == Tool::RECTANGLE) {
    // The pixels whose centres the rectangle covers.
    const QRectF rect(QRectF(m_dragStart, m_dragEnd).normalized());
    const QRect pixels(QPoint(int(std::ceil(rect.left() - 0.5)), int(std::ceil(rect.top() - 0.5))),
                       QPoint(int(std::floor(rect.right() - 0.5)), int(std::floor(rect.bottom() - 0.5))));
    const QRect clipped(pixels.intersected(m_source.rect()));
    if (pixels.isValid() && !clipped.isEmpty()) {
      commit(Edit::rect(clipped, Qt::white));
    }
  } else if ((m_tool == Tool::BRUSH) && !m_stroke.isEmpty()) {
    commit(Edit::stroke(m_stroke, m_brushDiameter, Qt::white));
  }
  m_stroke.clear();
  m_dragStart = m_dragEnd = QPointF();
  event->accept();
  update();
}

void RetouchView::onKeyPressEvent(QKeyEvent* event, InteractionState&) {
  if ((event->key() == Qt::Key_Space) && !event->isAutoRepeat()) {
    m_spaceDown = true;
    updateToolCursor();
    event->accept();
  }
}

void RetouchView::onKeyReleaseEvent(QKeyEvent* event, InteractionState&) {
  if ((event->key() == Qt::Key_Space) && !event->isAutoRepeat()) {
    m_spaceDown = false;
    updateToolCursor();
    event->accept();
  }
}

void RetouchView::leaveEvent(QEvent* event) {
  m_cursorInside = false;
  update();
  ImageViewBase::leaveEvent(event);
}

void RetouchView::pickColorAt(const QPointF& imagePos) {
  const QPoint pixel(int(std::floor(imagePos.x())), int(std::floor(imagePos.y())));
  if (!m_source.rect().contains(pixel)) {
    return;
  }
  // The median of a 7x7 square: a single pixel of a scan is grain and dust.
  const QColor color(sampleColor(m_source, pixel, 3));
  if (color.isValid()) {
    emit colorPicked(color);
  }
}

void RetouchView::selectAt(const QPointF& imagePos, const Qt::KeyboardModifiers modifiers) {
  const QPoint seed(int(std::floor(imagePos.x())), int(std::floor(imagePos.y())));
  if (!m_source.rect().contains(seed)) {
    return;
  }
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const Selection found(selectObject(m_source, seed, m_selectionParams));
  QApplication::restoreOverrideCursor();

  if (modifiers & Qt::ShiftModifier) {
    m_selection = m_selection.united(found);
  } else if (modifiers & Qt::ControlModifier) {
    m_selection = m_selection.subtracted(found);
  } else {
    m_selection = found;
  }
  if (found.isNull()) {
    emit message(tr("No ink to select there. Click on the stamp or mark itself, or lower the tolerance."));
  }
  m_selectionImage = m_selection.isNull() ? QImage() : m_selection.mask().toAlphaMask(QColor(0, 150, 255, 130));
  emit selectionChanged();
  update();
}

void RetouchView::commit(const Edit& shape) {
  QColor color(Qt::white);
  bool fromPaper = false;
  switch (m_fillMode) {
    case FillMode::WHITE:
      break;
    case FillMode::PICKED:
      if (m_pickedColor.isValid()) {
        color = m_pickedColor;
      }
      break;
    case FillMode::AUTO: {
      // Worked out now, once, so that what is shown is what gets saved.
      const QRect area(shape.boundingRect().intersected(m_source.rect()));
      if (!area.isEmpty()) {
        const QColor paper(surroundingColor(m_source, shape.coverage(area), area.topLeft(), m_paperRing));
        if (paper.isValid()) {
          color = paper;
          fromPaper = true;
        }
      }
      break;
    }
  }

  const Edit edit(shape.withColor(color, fromPaper));
  // A new edit ends the redo history, as everywhere.
  m_edits.erase(m_edits.begin() + m_applied, m_edits.end());
  m_previewColors.erase(m_previewColors.begin() + m_applied, m_previewColors.end());
  m_maskImages.erase(m_maskImages.begin() + m_applied, m_maskImages.end());
  m_edits.push_back(edit);
  m_previewColors.push_back(effectiveColor(color));
  m_maskImages.push_back((edit.kind() == Edit::MASK) ? edit.maskImage().toAlphaMask(m_previewColors.back()) : QImage());
  ++m_applied;
  emit editsChanged();
  update();
}

QColor RetouchView::previewColor() const {
  switch (m_fillMode) {
    case FillMode::WHITE:
      return effectiveColor(Qt::white);
    case FillMode::PICKED:
      return effectiveColor(m_pickedColor.isValid() ? m_pickedColor : QColor(Qt::white));
    case FillMode::AUTO:
      break;
  }
  // The paper colour is only known once the shape is: a neutral tint until then.
  return QColor(128, 128, 128, 150);
}

void RetouchView::paintEdits(QPainter& painter) const {
  for (int i = 0; i < m_applied; ++i) {
    const Edit& edit = m_edits[size_t(i)];
    const QColor& color = m_previewColors[size_t(i)];
    switch (edit.kind()) {
      case Edit::RECT:
        painter.fillRect(edit.rect(), color);
        break;
      case Edit::STROKE:
        paintStroke(painter, edit.points(), edit.diameter(), color);
        break;
      case Edit::MASK:
        painter.drawImage(edit.maskOrigin(), m_maskImages[size_t(i)]);
        break;
    }
  }
}

/*=============================== RetouchMirror ===============================*/

RetouchMirror::RetouchMirror(RetouchView* source, ImageViewBase* target, const QTransform& sourceToTarget)
    : m_source(source), m_target(target), m_sourceToTarget(sourceToTarget) {}

void RetouchMirror::onPaint(QPainter& painter, const InteractionState&) {
  if (!m_source || !m_target) {
    return;
  }
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
  painter.setWorldTransform(m_sourceToTarget * m_target->imageToVirtual() * m_target->virtualToWidget());
  m_source->paintEdits(painter);
}

void RetouchView::updateToolCursor() {
  QCursor cursor(Qt::CrossCursor);
  QString tip;
  if (m_spaceDown) {
    cursor = QCursor(Qt::OpenHandCursor);
    tip = tr("Drag to move the image.");
  } else {
    switch (m_tool) {
      case Tool::RECTANGLE:
        tip = tr(
            "Drag a rectangle to fill it. The middle button, or Space with a drag, moves the image. "
            "Alt+click picks the colour.");
        break;
      case Tool::BRUSH:
        tip = tr("Paint over the image. [ and ] change the brush size. Alt+click picks the colour.");
        break;
      case Tool::SELECT:
        cursor = QCursor(Qt::PointingHandCursor);
        tip = tr(
            "Click a stamp or mark to select it; Shift+click adds to the selection, Ctrl+click takes away. "
            "Delete fills it.");
        break;
      case Tool::PICK:
        tip = tr("Click the paper to take its colour.");
        break;
    }
  }
  m_hoverCaptor.setProximityCursor(cursor);
  m_hoverCaptor.setProximityStatusTip(tip);
  m_paintCaptor.setInteractionCursor(cursor);
  m_paintCaptor.setInteractionStatusTip(tip);
  ensureStatusTip(tip);
  viewport()->setCursor(cursor);
}

void RetouchView::onPaint(QPainter& painter, const InteractionState& interaction) {
  // Hard edges, as the pixels will be written: no antialiasing, no smoothing.
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
  painter.setWorldTransform(imageToVirtual() * virtualToWidget());
  // What reaches past the image is cut off in the file too.
  painter.setClipRect(m_source.rect());

  paintEdits(painter);

  if (!m_selectionImage.isNull()) {
    painter.drawImage(m_selection.origin(), m_selectionImage);
  }

  const bool drawing = interaction.capturedBy(m_paintCaptor);
  QRectF dragRect;
  if (drawing && (m_tool == Tool::RECTANGLE)) {
    dragRect = QRectF(m_dragStart, m_dragEnd).normalized();
    painter.fillRect(dragRect, previewColor());
  } else if (drawing && (m_tool == Tool::BRUSH)) {
    paintStroke(painter, m_stroke, m_brushDiameter, previewColor());
  }
  painter.setClipping(false);

  // Guides, in screen pixels.
  painter.setWorldTransform(QTransform());
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setBrush(Qt::NoBrush);
  if (!dragRect.isNull()) {
    painter.setPen(QPen(QColor(0, 150, 255), 1.0, Qt::DashLine));
    painter.drawRect(imageToWidget().mapRect(dragRect));
  }
  if ((m_tool == Tool::BRUSH) && m_cursorInside && !panning()) {
    const double scale = QLineF(imageToWidget().map(QLineF(0, 0, 1, 0))).length();
    const double radius = std::max(1.0, m_brushDiameter * scale / 2.0);
    // Light under dark, so that it shows on white paper and on black ink alike.
    painter.setPen(QPen(QColor(255, 255, 255, 220), 2.0));
    painter.drawEllipse(m_cursorPos, radius, radius);
    painter.setPen(QPen(QColor(0, 0, 0, 220), 1.0));
    painter.drawEllipse(m_cursorPos, radius, radius);
  }
}
