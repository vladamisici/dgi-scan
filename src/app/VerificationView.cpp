// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "VerificationView.h"

#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <memory>
#include <utility>

#include "AbstractCommand.h"
#include "BackgroundExecutor.h"
#include "BasicImageView.h"
#include "ImageId.h"
#include "ImageLoader.h"
#include "ImageViewBase.h"

namespace {
QWidget* panelWithHeader(const QString& title, QWidget* content, const bool editable, QWidget* parent) {
  auto* panel = new QFrame(parent);
  panel->setObjectName(QLatin1String("verificationPanel"));

  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* header = new QLabel(title, panel);
  header->setAlignment(Qt::AlignCenter);
  header->setObjectName(editable ? QLatin1String("verificationEditableHeader")
                                 : QLatin1String("verificationOriginalHeader"));
  header->setStyleSheet(editable
                            ? QLatin1String("QLabel { color: palette(highlighted-text); background: palette(highlight); "
                                            "font-weight: bold; padding: 5px; }")
                            : QLatin1String("QLabel { color: palette(window-text); background: palette(alternate-base); "
                                            "font-weight: bold; padding: 5px; }"));
  layout->addWidget(header);
  layout->addWidget(content, 1);
  return panel;
}
}  // namespace

class VerificationView::ImageLoadResult : public AbstractCommand<void> {
 public:
  ImageLoadResult(QPointer<VerificationView> owner, QImage image, QImage downscaled)
      : m_owner(std::move(owner)), m_image(std::move(image)), m_downscaled(std::move(downscaled)) {}

  void operator()() override {
    if (VerificationView* owner = m_owner) {
      owner->originalLoaded(m_image, m_downscaled);
    }
  }

 private:
  QPointer<VerificationView> m_owner;
  QImage m_image;
  QImage m_downscaled;
};

class VerificationView::ImageLoaderTask : public AbstractCommand<BackgroundExecutor::TaskResultPtr> {
 public:
  ImageLoaderTask(VerificationView* owner, const ImageId& imageId) : m_owner(owner), m_imageId(imageId) {}

  BackgroundExecutor::TaskResultPtr operator()() override {
    QImage image = ImageLoader::load(m_imageId);
    // Downscaled here, on the background thread. Done in originalLoaded(), it
    // ran on the GUI thread - an area average over the whole original, after a
    // full 1-bit to 8-bit conversion for bitonal scans - on every page shown.
    QImage downscaled;
    if (!image.isNull()) {
      downscaled = ImageViewBase::createDownscaledImage(image);
      if (ImageViewBase::lowResDisplay()) {
        // Nothing is edited on this side, so no coordinates depend on the full
        // resolution: the reduced copy can stand in for it altogether.
        image = downscaled;
      }
    }
    return std::make_shared<ImageLoadResult>(m_owner, image, downscaled);
  }

 private:
  QPointer<VerificationView> m_owner;
  ImageId m_imageId;
};

VerificationView::VerificationView(QWidget* projectView,
                                   const ImageId& originalImage,
                                   const QString& projectImagePath,
                                   QWidget* parent)
    : QWidget(parent), m_originalStack(new QStackedWidget(this)), m_loadingWidget(new QLabel(this)) {
  auto* loadingLabel = static_cast<QLabel*>(m_loadingWidget);
  loadingLabel->setAlignment(Qt::AlignCenter);
  loadingLabel->setWordWrap(true);
  loadingLabel->setText(tr("Loading the original image..."));
  m_originalStack->addWidget(m_loadingWidget);

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setChildrenCollapsible(false);
  splitter->addWidget(panelWithHeader(tr("INPUT - READ ONLY"), m_originalStack, false, splitter));
  splitter->addWidget(panelWithHeader(tr("PROJECT - EDITABLE"), projectView, true, splitter));
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({1, 1});

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(splitter);

  setToolTip(projectImagePath);
  if (originalImage.isNull()) {
    showOriginalMessage(
        tr("No unambiguous matching original was found in the selected input folders.\n\nProject image:\n%1")
            .arg(projectImagePath));
  } else {
    ImageViewBase::backgroundExecutor().enqueueTask(std::make_shared<ImageLoaderTask>(this, originalImage));
  }
}

void VerificationView::originalLoaded(const QImage& image, const QImage& downscaled) {
  if (image.isNull()) {
    showOriginalMessage(tr("The matching original image could not be opened."));
    return;
  }

  auto* view = new BasicImageView(image, downscaled, Margins());
  m_originalStack->setCurrentIndex(m_originalStack->addWidget(view));
}

void VerificationView::showOriginalMessage(const QString& message) {
  auto* label = static_cast<QLabel*>(m_loadingWidget);
  label->setText(message);
  label->setToolTip(message);
  m_originalStack->setCurrentWidget(label);
}
