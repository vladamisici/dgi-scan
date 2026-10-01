// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_RETOUCHVIEW_H_
#define SCANTAILOR_APP_RETOUCHVIEW_H_

#include <Edit.h>
#include <Selection.h>

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QPolygonF>
#include <QTransform>
#include <vector>

#include "DragHandler.h"
#include "ImageViewBase.h"
#include "InteractionHandler.h"
#include "InteractionState.h"
#include "RetouchTypes.h"
#include "ZoomHandler.h"

class ImageTransformation;

/**
 * \brief Shows a page's source image and lets the operator paint over it.
 *
 * The image is shown the way the rest of the application shows it - turned by
 * the page's orientation - while every change is recorded in the pixel
 * coordinates of the file, as an Edit. Edits are drawn over the image rather
 * than painted into it, which makes undo free and keeps the image itself
 * untouched until the operator saves.
 *
 * The left button paints with the current tool. The middle button, or the left
 * one with Space held down, drags the image; the wheel zooms, as everywhere
 * else. Alt with a click picks the fill colour from the image whatever the tool.
 */
class RetouchView : public ImageViewBase, private InteractionHandler {
  Q_OBJECT
 public:
  /**
   * \param source The image as loaded from the file, in its own format.
   *        Edits are measured against it, and colours are picked from it.
   * \param display \p source converted for display, with the project's DPI.
   * \param downscaled A reduced copy of \p display.
   * \param xform Places the image as the project shows it.
   */
  RetouchView(const QImage& source, const QImage& display, const QImage& downscaled, const ImageTransformation& xform);

  ~RetouchView() override;

  const QImage& sourceImage() const { return m_source; }

  void setTool(retouch::Tool tool);

  void setFillMode(retouch::FillMode mode, const QColor& pickedColor);

  void setBrushDiameter(int pixels);

  void setSelectionParams(const retouch::ObjectSelectionParams& params);

  /** \brief The edits in effect: those made, less those undone. */
  std::vector<retouch::Edit> edits() const;

  int editCount() const { return m_applied; }

  bool canUndo() const { return m_applied > 0; }

  bool canRedo() const { return m_applied < static_cast<int>(m_edits.size()); }

  void undo();

  void redo();

  bool hasSelection() const { return !m_selection.isNull(); }

  /** \brief Turns the selection into an edit filled with the current colour. */
  void fillSelection();

  void clearSelection();

  /** \brief The colour \p requested comes out as in this image's format. */
  QColor effectiveColor(const QColor& requested) const;

  /** \brief Draws the edits in effect, in the pixel coordinates of the image, with \p painter set up for them. */
  void paintEdits(QPainter& painter) const;

  /** \brief The width, in pixels, of the band around a change that its paper colour is taken from. */
  int paperRing() const { return m_paperRing; }

 signals:
  /** \brief An edit was made, undone or redone. */
  void editsChanged();

  void selectionChanged();

  /** \brief The operator took \p color from the image. */
  void colorPicked(const QColor& color);

  /** \brief Something worth telling the operator, for the status bar. */
  void message(const QString& text);

 private:
  void onPaint(QPainter& painter, const InteractionState& interaction) override;

  void onProximityUpdate(const QPointF& screenMousePos, InteractionState& interaction) override;

  void onMousePressEvent(QMouseEvent* event, InteractionState& interaction) override;

  void onMouseMoveEvent(QMouseEvent* event, InteractionState& interaction) override;

  void onMouseReleaseEvent(QMouseEvent* event, InteractionState& interaction) override;

  void onKeyPressEvent(QKeyEvent* event, InteractionState& interaction) override;

  void onKeyReleaseEvent(QKeyEvent* event, InteractionState& interaction) override;

  void leaveEvent(QEvent* event) override;

  QPointF toImage(const QPointF& widgetPos) const;

  bool panning() const;

  void pickColorAt(const QPointF& imagePos);

  void selectAt(const QPointF& imagePos, Qt::KeyboardModifiers modifiers);

  /** \brief Adds \p edit with the fill colour, working out the paper colour if that is the mode. */
  void commit(const retouch::Edit& shape);

  void updateToolCursor();

  /** \brief The colour previews are drawn in: the colour itself, or a neutral tint when it is still to be worked out.
   */
  QColor previewColor() const;

  QImage m_source;
  DragHandler m_dragHandler;
  ZoomHandler m_zoomHandler;
  InteractionState::Captor m_paintCaptor;
  InteractionState::Captor m_hoverCaptor;

  std::vector<retouch::Edit> m_edits;
  /** The colour each edit shows on screen, which is what the file can hold. */
  std::vector<QColor> m_previewColors;
  /** For mask edits, the mask coloured in, ready to draw. */
  std::vector<QImage> m_maskImages;
  /** How many of m_edits are in effect; the rest are undone, kept for redo. */
  int m_applied;

  retouch::Selection m_selection;
  QImage m_selectionImage;

  retouch::Tool m_tool;
  retouch::FillMode m_fillMode;
  QColor m_pickedColor;
  int m_brushDiameter;
  retouch::ObjectSelectionParams m_selectionParams;
  /** The width of the band sampled for the paper colour, in image pixels. */
  int m_paperRing;

  /** Where the drag of the current rectangle started, and where it is now, in image pixels. */
  QPointF m_dragStart;
  QPointF m_dragEnd;
  QPolygonF m_stroke;
  QPointF m_cursorPos;
  bool m_cursorInside;
  bool m_spaceDown;
};


/**
 * \brief Shows the edits made in a RetouchView over another view of the same page.
 *
 * In verification mode the input image is edited on the left while the
 * project's own copy of it is shown on the right, and saving changes both. This
 * draws the edits on the right as they will fall there, so that the operator
 * sees where they land before saving.
 *
 * Linked into the other view's interaction handlers, which own it from then on:
 * it goes when that view goes.
 */
class RetouchMirror : public InteractionHandler {
 public:
  /**
   * \param sourceToTarget From the pixels of the image \p source edits to those
   *        of the image \p target shows.
   */
  RetouchMirror(RetouchView* source, ImageViewBase* target, const QTransform& sourceToTarget);

 protected:
  void onPaint(QPainter& painter, const InteractionState& interaction) override;

 private:
  QPointer<RetouchView> m_source;
  QPointer<ImageViewBase> m_target;
  QTransform m_sourceToTarget;
};


#endif  // SCANTAILOR_APP_RETOUCHVIEW_H_
