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
 * Side-by-side "compare with input" workspace.
 *
 * The left side is the page's image from the input folders, read-only unless
 * it is being retouched: setInputEditor() puts an editor in its place. The
 * right side is the regular ScanTailor view, unchanged, so every interaction
 * and save continues through the normal project pipeline. Each side's header
 * names the folder its image comes from.
 */
class VerificationView : public QWidget {
 public:
  /**
   * \param missingMessage Shown on the input side when \p originalImage is
   *        null: why there is no input image for this page.
   */
  VerificationView(QWidget* projectView,
                   const ImageId& originalImage,
                   const QString& projectImagePath,
                   const QString& missingMessage,
                   QWidget* parent = nullptr);

  /**
   * \brief Shows \p editor in place of the input image, under \p title.
   *
   * Takes ownership of \p editor. The input image is not shown again: the view
   * is rebuilt once the editing is over.
   */
  void setInputEditor(QWidget* editor, const QString& title);

  /** \brief What the right side's header says: \p title, and the folder of \p imagePath. */
  void setProjectHeader(const QString& title, const QString& imagePath);

  /**
   * \brief Shows \p editor in place of the project's view, under \p title.
   *
   * Takes ownership of \p editor. The view is rebuilt once the editing is over.
   */
  void setProjectEditor(QWidget* editor, const QString& title);

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
  QStackedWidget* m_projectStack;
  QWidget* m_loadingWidget;
  QLabel* m_inputHeader;
  QLabel* m_projectHeader;
  QWidget* m_projectView;
  QString m_projectImagePath;
  /** The input image's folder as its header names it, separator included; empty without an image. */
  QString m_inputFolder;
  /** The same for the right side. */
  QString m_projectFolder;
  /** Set once an editor has taken the input's place, which a late load must not undo. */
  bool m_editing;
};

#endif  // SCANTAILOR_APP_VERIFICATIONVIEW_H_
