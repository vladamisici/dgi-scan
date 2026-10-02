// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "VerificationView.h"

#include <core/IconProvider.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include "AbstractCommand.h"
#include "BackgroundExecutor.h"
#include "BasicImageView.h"
#include "ImageLoader.h"
#include "ImageViewBase.h"
#include "ProjectHistory.h"

namespace {
const char MINIMIZED_KEY[] = "compare/inputMinimized";
// The input side's share of the width the two sides have between them.
const char SHARE_KEY[] = "compare/inputShare";

void styleHeaderBar(QFrame* bar, const bool editable) {
  bar->setStyleSheet(editable ? QLatin1String("QFrame#verificationHeaderBar { background: palette(highlight); } "
                                              "QFrame#verificationHeaderBar QLabel { color: palette(highlighted-text); "
                                              "font-weight: bold; }")
                              : QLatin1String("QFrame#verificationHeaderBar { background: palette(alternate-base); } "
                                              "QFrame#verificationHeaderBar QLabel { color: palette(window-text); "
                                              "font-weight: bold; }"));
}

/** A bar with \p title in the middle, in \p bar, over \p content; the bar's layout takes buttons at its end. */
QWidget* panelWithHeader(const QString& title,
                         QWidget* content,
                         const bool editable,
                         QWidget* parent,
                         QLabel** headerOut,
                         QFrame** barOut = nullptr) {
  auto* panel = new QFrame(parent);
  panel->setObjectName(QLatin1String("verificationPanel"));

  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* bar = new QFrame(panel);
  bar->setObjectName(QLatin1String("verificationHeaderBar"));
  styleHeaderBar(bar, editable);
  auto* barLayout = new QHBoxLayout(bar);
  barLayout->setContentsMargins(5, 2, 2, 2);
  barLayout->setSpacing(2);
  auto* header = new QLabel(title, bar);
  header->setAlignment(Qt::AlignCenter);
  header->setMinimumHeight(22);
  barLayout->addWidget(header, 1);

  layout->addWidget(bar);
  layout->addWidget(content, 1);
  *headerOut = header;
  if (barOut) {
    *barOut = bar;
  }
  return panel;
}

/** A separator and the last folders of \p imagePath, for a header naming where it is; empty without one. */
QString folderSuffix(const QString& imagePath) {
  return imagePath.isEmpty() ? QString()
                             : QLatin1String("   ") + QChar(0x00B7) + QLatin1String("   ")
                                   + core::ProjectHistory::shortLocation(imagePath);
}

/** The minimized input side: a narrow strip, its name written up it, that brings the input back when clicked. */
class InputStrip : public QFrame {
 public:
  InputStrip(const QString& caption, std::function<void()> restore, QWidget* parent)
      : QFrame(parent), m_caption(caption), m_restore(std::move(restore)) {
    setObjectName(QLatin1String("verificationInputStrip"));
    setStyleSheet(QLatin1String("QFrame#verificationInputStrip { background: palette(alternate-base); }"));
    setCursor(Qt::PointingHandCursor);
    setFixedWidth(26);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 2, 1, 2);
    layout->setSpacing(0);
    auto* button = new QToolButton(this);
    button->setIcon(IconProvider::getInstance().getIcon("panel-expand"));
    button->setAutoRaise(true);
    button->setToolTip(QWidget::tr("Show the input again"));
    button->setAccessibleName(QWidget::tr("Show input"));
    QObject::connect(button, &QToolButton::clicked, this, [this]() { m_restore(); });
    layout->addWidget(button, 0, Qt::AlignHCenter);
    layout->addStretch(1);
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QFrame::paintEvent(event);
    QPainter painter(this);
    QFont font(painter.font());
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(palette().color(QPalette::WindowText));
    // Written up the strip, below the button.
    const int top = 32;
    painter.translate(width() / 2.0 + painter.fontMetrics().ascent() / 2.0 - 1, height() - 8);
    painter.rotate(-90);
    const QString text(painter.fontMetrics().elidedText(m_caption, Qt::ElideRight, std::max(0, height() - top - 8)));
    painter.drawText(QPointF(0, 0), text);
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      m_restore();
    }
  }

 private:
  QString m_caption;
  std::function<void()> m_restore;
};
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
      m_splitter(nullptr),
      m_inputPanel(nullptr),
      m_inputStrip(nullptr),
      m_inputHeaderBar(nullptr),
      m_inputHeader(nullptr),
      m_projectHeader(nullptr),
      m_projectView(projectView),
      m_projectImagePath(projectImagePath),
      m_originalImage(originalImage),
      m_originalRequested(false),
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

  m_splitter = new QSplitter(Qt::Horizontal, this);
  m_splitter->setChildrenCollapsible(false);
  m_inputPanel = panelWithHeader(tr("INPUT - READ ONLY") + m_inputFolder, m_originalStack, false, m_splitter,
                                 &m_inputHeader, &m_inputHeaderBar);
  auto* minimizeButton = new QToolButton(m_inputHeaderBar);
  minimizeButton->setIcon(IconProvider::getInstance().getIcon("panel-collapse"));
  minimizeButton->setAutoRaise(true);
  minimizeButton->setToolTip(tr("Minimize the input to a strip at the left"));
  minimizeButton->setAccessibleName(tr("Minimize input"));
  connect(minimizeButton, &QToolButton::clicked, this, [this]() { setInputMinimized(true); });
  m_inputHeaderBar->layout()->addWidget(minimizeButton);
  m_splitter->addWidget(m_inputPanel);
  m_splitter->addWidget(
      panelWithHeader(tr("PROJECT - EDITABLE") + m_projectFolder, m_projectStack, true, m_splitter, &m_projectHeader));
  m_splitter->setStretchFactor(0, 1);
  m_splitter->setStretchFactor(1, 1);
  // The share the operator last gave the input. Relative sizes: the splitter
  // spreads them over whatever width it gets.
  const double share = std::clamp(QSettings().value(QLatin1String(SHARE_KEY), 0.5).toDouble(), 0.1, 0.9);
  m_splitter->setSizes({qRound(share * 10000), qRound((1.0 - share) * 10000)});
  connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
    const QList<int> sizes(m_splitter->sizes());
    if ((sizes.size() == 2) && (sizes[0] > 0) && (sizes[1] > 0)) {
      QSettings().setValue(QLatin1String(SHARE_KEY), double(sizes[0]) / (sizes[0] + sizes[1]));
    }
  });

  m_inputStrip = new InputStrip(tr("INPUT") + m_inputFolder, [this]() { setInputMinimized(false); }, this);
  m_inputStrip->setToolTip(tr("The input, minimized. Click to show it again."));

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(m_inputStrip);
  layout->addWidget(m_splitter, 1);

  setToolTip(projectImagePath);
  if (originalImage.isNull()) {
    showOriginalMessage(missingMessage);
  }
  const bool minimized = QSettings().value(QLatin1String(MINIMIZED_KEY), false).toBool();
  m_inputPanel->setVisible(!minimized);
  m_inputStrip->setVisible(minimized);
  if (!minimized) {
    loadOriginal();
  }
}

void VerificationView::setInputMinimized(const bool minimized) {
  QSettings().setValue(QLatin1String(MINIMIZED_KEY), minimized);
  m_inputStrip->setVisible(minimized);
  m_inputPanel->setVisible(!minimized);
  if (!minimized) {
    // Back at the width it had: a view made while it was minimized never gave it one.
    const double share = std::clamp(QSettings().value(QLatin1String(SHARE_KEY), 0.5).toDouble(), 0.1, 0.9);
    m_splitter->setSizes({qRound(share * 10000), qRound((1.0 - share) * 10000)});
    loadOriginal();
  }
}

void VerificationView::loadOriginal() {
  if (m_originalRequested || m_originalImage.isNull()) {
    return;
  }
  m_originalRequested = true;
  ImageViewBase::backgroundExecutor().enqueueTask(std::make_shared<ImageLoaderTask>(this, m_originalImage));
}

void VerificationView::setInputEditor(QWidget* editor, const QString& title) {
  m_editing = true;
  setInputMinimized(false);
  m_originalStack->setCurrentIndex(m_originalStack->addWidget(editor));
  m_inputHeader->setText(title + m_inputFolder);
  styleHeaderBar(m_inputHeaderBar, true);
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
