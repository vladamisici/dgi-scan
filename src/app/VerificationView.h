// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_VERIFICATIONVIEW_H_
#define SCANTAILOR_APP_VERIFICATIONVIEW_H_

#include <QWidget>

class ImageId;
class QImage;
class QLabel;
class QStackedWidget;

/**
 * Side-by-side verification workspace.
 *
 * The left side is the image loaded from the operator-selected input folders,
 * read-only unless it is being retouched: setInputEditor() puts an editor in
 * its place. The right side is the regular ScanTailor view, unchanged, so every
 * interaction and save continues through the normal project pipeline.
 */
class VerificationView : public QWidget {
 public:
  VerificationView(QWidget* projectView,
                   const ImageId& originalImage,
                   const QString& projectImagePath,
                   QWidget* parent = nullptr);

  /**
   * \brief Shows \p editor in place of the input image, under \p title.
   *
   * Takes ownership of \p editor. The input image is not shown again: the view
   * is rebuilt once the editing is over.
   */
  void setInputEditor(QWidget* editor, const QString& title);

  /** \brief The project's image this view was made for. */
  const QString& projectImagePath() const { return m_projectImagePath; }

  /** \brief The project's view of the page, on the right. */
  QWidget* projectView() const { return m_projectView; }

 private:
  class ImageLoadResult;
  class ImageLoaderTask;

  void originalLoaded(const QImage& image, const QImage& downscaled);
  void showOriginalMessage(const QString& message);

  QStackedWidget* m_originalStack;
  QWidget* m_loadingWidget;
  QLabel* m_inputHeader;
  QWidget* m_projectView;
  QString m_projectImagePath;
  /** Set once an editor has taken the input's place, which a late load must not undo. */
  bool m_editing;
};

#endif  // SCANTAILOR_APP_VERIFICATIONVIEW_H_
