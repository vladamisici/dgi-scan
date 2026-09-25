// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_VERIFICATIONVIEW_H_
#define SCANTAILOR_APP_VERIFICATIONVIEW_H_

#include <QWidget>

class ImageId;
class QImage;
class QStackedWidget;

/**
 * Side-by-side verification workspace.
 *
 * The left side is a read-only image loaded from the operator-selected input
 * folders. The right side is the regular ScanTailor view, unchanged, so every
 * interaction and save continues through the normal project pipeline.
 */
class VerificationView : public QWidget {
 public:
  VerificationView(QWidget* projectView,
                   const ImageId& originalImage,
                   const QString& projectImagePath,
                   QWidget* parent = nullptr);

 private:
  class ImageLoadResult;
  class ImageLoaderTask;

  void originalLoaded(const QImage& image, const QImage& downscaled);
  void showOriginalMessage(const QString& message);

  QStackedWidget* m_originalStack;
  QWidget* m_loadingWidget;
};

#endif  // SCANTAILOR_APP_VERIFICATIONVIEW_H_
