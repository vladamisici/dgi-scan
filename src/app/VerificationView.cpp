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
#include "ProjectHistory.h"

namespace {
const char EDITABLE_HEADER_STYLE[]
    = "QLabel { color: palette(highlighted-text); background: palette(highlight); font-weight: bold; padding: 5px; }";

QWidget* panelWithHeader(const QString& title,
                         QWidget* content,
                         const bool editable,
                         QWidget* parent,
                         QLabel** headerOut = nullptr) {
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
                            ? QLatin1String(EDITABLE_HEADER_STYLE)
                            : QLatin1String("QLabel { color: palette(window-text); background: palette(alternate-base); "
                                            "font-weight: bold; padding: 5px; }"));
  layout->addWidget(header);
  layout->addWidget(content, 1);
  if (headerOut) {
    *headerOut = header;
  }
  return panel;
}

/** A separator and the last folders of \p imagePath, for a header naming where it is; empty without one. */
QString folderSuffix(const QString& imagePath) {
  return imagePath.isEmpty() ? QString()
                             : QLatin1String("   ") + QChar(0x00B7) + QLatin1String("   ")
                                   + core::ProjectHistory::shortLocation(imagePath);
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
                                   const QString& missingMessage,
                                   QWidget* parent)
    : QWidget(parent),
      m_originalStack(new QStackedWidget(this)),
      m_projectStack(new QStackedWidget(this)),
      m_loadingWidget(new QLabel(this)),
      m_inputHeader(nullptr),
      m_projectHeader(nullptr),
      m_projectView(projectView),
      m_projectImagePath(projectImagePath),
      m_editing(false) {
  auto* loadingLabel = static_cast<QLabel*>(m_loadingWidget);
  loadingLabel->setAlignment(Qt::AlignCenter);
  loadingLabel->setWordWrap(true);
  loadingLabel->setText(tr("Loading the original image..."));
  m_originalStack->addWidget(m_loadingWidget);

  // Each side names where its image comes from: all titles' folders look the
  // same, and a comparison with another title's folder must show at a glance.
  m_inputFolder = folderSuffix(originalImage.filePath());
  m_projectFolder = folderSuffix(projectImagePath);
  m_projectStack->addWidget(projectView);
  auto* splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setChildrenCollapsible(false);
  splitter->addWidget(
      panelWithHeader(tr("INPUT - READ ONLY") + m_inputFolder, m_originalStack, false, splitter, &m_inputHeader));
  splitter->addWidget(panelWithHeader(tr("PROJECT - EDITABLE") + m_projectFolder, m_projectStack, true, splitter,
                                      &m_projectHeader));
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({1, 1});

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(splitter);

  setToolTip(projectImagePath);
  if (originalImage.isNull()) {
    showOriginalMessage(missingMessage);
  } else {
    ImageViewBase::backgroundExecutor().enqueueTask(std::make_shared<ImageLoaderTask>(this, originalImage));
  }
}

void VerificationView::setInputEditor(QWidget* editor, const QString& title) {
  m_editing = true;
  m_originalStack->setCurrentIndex(m_originalStack->addWidget(editor));
  m_inputHeader->setText(title + m_inputFolder);
  m_inputHeader->setStyleSheet(QLatin1String(EDITABLE_HEADER_STYLE));
  editor->setFocus();
}

void VerificationView::setProjectHeader(const QString& title, const QString& imagePath) {
  m_projectFolder = folderSuffix(imagePath);
  m_projectHeader->setText(title + m_projectFolder);
}

void VerificationView::setProjectEditor(QWidget* editor, const QString& title) {
  m_projectStack->setCurrentIndex(m_projectStack->addWidget(editor));
  m_projectHeader->setText(title + m_projectFolder);
  editor->setFocus();
}

void VerificationView::originalLoaded(const QImage& image, const QImage& downscaled) {
  if (m_editing) {
    // The editor shows the same file; the read-only copy would take its place.
    return;
  }
  if (image.isNull()) {
    showOriginalMessage(tr("The matching original image could not be opened."));
    return;
  }

  auto* view = new BasicImageView(image, downscaled, Margins());
  m_originalStack->setCurrentIndex(m_originalStack->addWidget(view));
}

void VerificationView::showOriginalMessage(const QString& message) {
  if (m_editing) {
    return;
  }
  auto* label = static_cast<QLabel*>(m_loadingWidget);
  label->setText(message);
  label->setToolTip(message);
  m_originalStack->setCurrentWidget(label);
}
