// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "RetouchController.h"

#include <Grayscale.h>
#include <OriginalsBackup.h>
#include <SourceFile.h>
#include <core/CrashHandler.h>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <cmath>
#include <exception>
#include <new>
#include <utility>

#include "AbstractCommand.h"
#include "BackgroundExecutor.h"
#include "Diagnostics.h"
#include "Dpm.h"
#include "ImageLoader.h"
#include "ImageTransformation.h"
#include "ImageViewBase.h"
#include "RetouchPanel.h"
#include "RetouchView.h"
#include "ThumbnailPixmapCache.h"

using namespace retouch;

namespace {
/**
 * \brief \p source as the rest of the application shows and thumbnails it.
 *
 * Grey or colour the way the processing stages see it, and at the project's
 * resolution, which may have been overridden from the file's.
 */
QImage displayForm(const QImage& source, const Dpi& dpi) {
  QImage display;
  if (((source.format() == QImage::Format_Indexed8) && !source.isGrayscale()) || (source.depth() > 8)) {
    display = source.convertToFormat(source.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
  } else {
    display = imageproc::toGrayscale(source);
  }
  if (!dpi.isNull()) {
    const Dpm dpm(dpi);
    display.setDotsPerMeterX(dpm.horizontal());
    display.setDotsPerMeterY(dpm.vertical());
  }
  return display;
}

/** The resolution to show \p image at: the target's, or failing that the file's, or failing that a sane one. */
Dpi targetDpi(const RetouchTarget& target, const QImage& image) {
  if (!target.dpi.isNull()) {
    return target.dpi;
  }
  const Dpi fromFile{Dpm(image)};
  return fromFile.isNull() ? Dpi(300, 300) : fromFile;
}

QString displayNameOf(const ImageId& imageId) {
  if (imageId.isNull()) {
    return QString();
  }
  const QString name(QFileInfo(imageId.filePath()).fileName());
  return imageId.isMultiPageFile() ? RetouchController::tr("%1, page %2").arg(name).arg(imageId.page()) : name;
}

/** Whether the file has changed since it was \p size bytes, last written at \p time. */
bool changedSince(const QString& path, const qint64 size, const QDateTime& time) {
  const QFileInfo info(path);
  return (info.size() != size) || (info.lastModified() != time);
}

void showStatus(QWidget* window, const QString& text, const int timeoutMs) {
  if (auto* mainWindow = qobject_cast<QMainWindow*>(window)) {
    mainWindow->statusBar()->showMessage(text, timeoutMs);
  }
}
}  // namespace

bool RetouchTarget::editsProjectImage() const {
  if (projectImageId.isNull()) {
    return false;
  }
  const auto spelling = [](const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); };
  return (imageId.page() == projectImageId.page())
         && (QString::compare(spelling(imageId.filePath()), spelling(projectImageId.filePath()), Qt::CaseInsensitive)
             == 0);
}

struct RetouchController::LoadedImage {
  QImage source;
  QImage display;
  QImage downscaled;
  QString unsupported;
  qint64 size = -1;
  QDateTime modified;
  /** For the project's separate copy, if there is one. */
  QString projectUnsupported;
  qint64 projectSize = -1;
  QDateTime projectModified;
};


class RetouchController::LoadResult : public AbstractCommand<void> {
 public:
  LoadResult(QPointer<RetouchController> owner, const int generation, std::shared_ptr<LoadedImage> image)
      : m_owner(std::move(owner)), m_generation(generation), m_image(std::move(image)) {}

  void operator()() override {
    if (RetouchController* owner = m_owner) {
      owner->loaded(m_generation, m_image);
    }
  }

 private:
  QPointer<RetouchController> m_owner;
  int m_generation;
  std::shared_ptr<LoadedImage> m_image;
};


/** Loads the image, and checks it can be written back, off the GUI thread: on a share either can take a while. */
class RetouchController::LoadTask : public AbstractCommand<BackgroundExecutor::TaskResultPtr> {
 public:
  LoadTask(QPointer<RetouchController> owner, const int generation, RetouchTarget target)
      : m_owner(std::move(owner)), m_generation(generation), m_target(std::move(target)) {}

  BackgroundExecutor::TaskResultPtr operator()() override {
    auto image = std::make_shared<LoadedImage>();
    const QFileInfo info(m_target.imageId.filePath());
    image->size = info.size();
    image->modified = info.lastModified();
    try {
      // The output is only read: what is painted on it is kept with the project.
      if (!m_target.output) {
        image->unsupported = SourceFile::unsupportedReason(m_target.imageId);
      }
      if (image->unsupported.isEmpty()) {
        image->source = ImageLoader::load(m_target.imageId);
        if (!image->source.isNull()) {
          image->display = displayForm(image->source, targetDpi(m_target, image->source));
          image->downscaled = ImageViewBase::createDownscaledImage(image->display);
        }
      }
      if (!m_target.projectImageId.isNull() && !m_target.editsProjectImage()) {
        const QFileInfo projectInfo(m_target.projectImageId.filePath());
        image->projectSize = projectInfo.size();
        image->projectModified = projectInfo.lastModified();
        image->projectUnsupported = SourceFile::unsupportedReason(m_target.projectImageId);
      }
    } catch (const std::bad_alloc&) {
      image->source = QImage();
      image->unsupported = RetouchController::tr("there is not enough memory to open it");
    }
    return std::make_shared<LoadResult>(m_owner, m_generation, image);
  }

 private:
  QPointer<RetouchController> m_owner;
  int m_generation;
  RetouchTarget m_target;
};


RetouchController::RetouchController(RetouchHost& host, RetouchPanel& panel, QWidget* window)
    : m_host(host),
      m_panel(panel),
      m_window(window),
      m_state(IDLE),
      m_generation(0),
      m_loadedSize(-1),
      m_carryOver(false),
      m_carryX(1.0),
      m_carryY(1.0),
      m_projectLoadedSize(-1),
      m_toolBeforePick(panel.tool()) {
  connect(&m_panel, &RetouchPanel::toolActivated, this, &RetouchController::toolActivated);
  connect(&m_panel, &RetouchPanel::fillChanged, this, [this]() {
    applyPanelSettings();
    updateEffectiveColor();
  });
  connect(&m_panel, &RetouchPanel::brushDiameterChanged, this, [this](const int pixels) {
    if (m_view) {
      m_view->setBrushDiameter(pixels);
    }
  });
  connect(&m_panel, &RetouchPanel::selectionParamsChanged, this, [this]() {
    if (m_view) {
      m_view->setSelectionParams(m_panel.selectionParams());
    }
  });
  connect(&m_panel, &RetouchPanel::undoRequested, this, [this]() {
    if (m_view) {
      m_view->undo();
    }
  });
  connect(&m_panel, &RetouchPanel::redoRequested, this, [this]() {
    if (m_view) {
      m_view->redo();
    }
  });
  connect(&m_panel, &RetouchPanel::fillSelectionRequested, this, [this]() {
    if (m_view) {
      m_view->fillSelection();
    }
  });
  connect(&m_panel, &RetouchPanel::clearSelectionRequested, this, [this]() {
    if (m_view) {
      m_view->clearSelection();
    }
  });
  connect(&m_panel, &RetouchPanel::saveRequested, this, [this]() {
    if (save()) {
      m_host.retouchSessionEnded();
    }
  });
  connect(&m_panel, &RetouchPanel::closeRequested, this, &RetouchController::closeRequested);
  connect(&m_panel, &RetouchPanel::restoreRequested, this, &RetouchController::restoreRequested);
  updatePanel();
}

RetouchController::~RetouchController() = default;

bool RetouchController::hasUnsavedEdits() const {
  return (m_state == EDITING) && m_view && (m_view->editCount() > 0);
}

QString RetouchController::displayName() const {
  return displayNameOf(m_target.imageId);
}

void RetouchController::toolActivated(const Tool tool) {
  if (tool != Tool::PICK) {
    m_toolBeforePick = tool;
  }
  if (m_state == IDLE) {
    begin(tool);
  } else if (m_view) {
    m_view->setTool(tool);
  }
}

void RetouchController::begin(const Tool) {
  if (!m_host.retouchAllowed()) {
    return;
  }
  const PageInfo page(m_host.retouchCurrentPage());
  if (page.isNull()) {
    return;
  }
  RetouchTarget target;
  QString whyNot;
  if (!m_host.retouchTarget(page, &target, &whyNot)) {
    QMessageBox::information(m_window, tr("Retouch"), whyNot);
    return;
  }
  m_page = page;
  m_target = target;
  m_state = LOADING;
  ++m_generation;
  updatePanel();
  ImageViewBase::backgroundExecutor().enqueueTask(std::make_shared<LoadTask>(this, m_generation, target));
}

void RetouchController::loaded(const int generation, const std::shared_ptr<LoadedImage>& image) {
  if ((generation != m_generation) || (m_state != LOADING)) {
    // The operator moved on while it loaded.
    return;
  }
  const QString name(displayName());
  if (!image->unsupported.isEmpty() || image->source.isNull()) {
    reset();
    updatePanel();
    QMessageBox::information(m_window, tr("Retouch"),
                             image->unsupported.isEmpty()
                                 ? tr("%1 could not be opened.").arg(name)
                                 : tr("%1 cannot be retouched: %2.").arg(name, image->unsupported));
    return;
  }

  // Whether, and how, the project's separate copy of the page gets the edits too.
  m_carryOver = false;
  m_carryX = 1.0;
  m_carryY = 1.0;
  m_carryNote.clear();
  if (!m_target.projectImageId.isNull() && !m_target.editsProjectImage()) {
    const QSize from(image->source.size());
    const QSize to(m_target.projectImageSize);
    const QString projectName(displayNameOf(m_target.projectImageId));
    if (!image->projectUnsupported.isEmpty()) {
      m_carryNote
          = tr("Only the input image will change: the project's copy cannot (%1).").arg(image->projectUnsupported);
    } else if (to.isEmpty()) {
      m_carryNote = tr("Only the input image will change: the size of the project's copy is not known.");
    } else {
      // The project's copy may be scaled - a different resolution - but must
      // be the same picture: the same shape, give or take rounding.
      const double sx = double(to.width()) / from.width();
      const double sy = double(to.height()) / from.height();
      if (std::abs(sx / sy - 1.0) > 0.01) {
        m_carryNote = tr("Only the input image will change: the project's copy is %1x%2 pixels, not the same "
                         "shape as the input's %3x%4, so the changes cannot be placed on it.")
                          .arg(to.width())
                          .arg(to.height())
                          .arg(from.width())
                          .arg(from.height());
      } else {
        m_carryOver = true;
        m_carryX = sx;
        m_carryY = sy;
      }
    }
    m_projectLoadedSize = image->projectSize;
    m_projectLoadedTime = image->projectModified;
  }

  ImageTransformation xform(QRectF(image->source.rect()), targetDpi(m_target, image->source));
  xform.setPreRotation(m_target.rotation);
  auto* view = new RetouchView(image->source, image->display, image->downscaled, xform);
  connect(view, &RetouchView::editsChanged, this, &RetouchController::updatePanel);
  connect(view, &RetouchView::selectionChanged, this, &RetouchController::updatePanel);
  connect(view, &RetouchView::colorPicked, this, [this](const QColor& color) {
    m_panel.setPickedColor(color);
    if (m_panel.tool() == Tool::PICK) {
      // Picking is a step on the way to painting: back to the tool it interrupted.
      m_panel.setTool(m_toolBeforePick);
      if (m_view) {
        m_view->setTool(m_toolBeforePick);
      }
    }
  });
  connect(view, &RetouchView::message, this, [this](const QString& text) { showStatus(m_window, text, 6000); });

  m_view = view;
  m_state = EDITING;
  m_loadedSize = image->size;
  m_loadedTime = image->modified;
  applyPanelSettings();
  m_host.retouchShowEditor(view, m_target.title, m_target.output);

  // The edits shown over the project's view of the page too, where they will
  // land when saved.
  if (m_carryOver || m_target.editsProjectImage()) {
    if (ImageViewBase* projectView = m_host.retouchProjectView()) {
      auto* mirror = new RetouchMirror(view, projectView, QTransform::fromScale(m_carryX, m_carryY));
      // Over the image, under the stage's own guides.
      projectView->rootInteractionHandler().makeFirstPreceeder(*mirror);
      connect(view, &RetouchView::editsChanged, projectView, [projectView]() { projectView->update(); });
    }
  }
  updatePanel();
  if (!m_carryNote.isEmpty()) {
    showStatus(m_window, m_carryNote, 15000);
  }
}

void RetouchController::applyPanelSettings() {
  if (!m_view) {
    return;
  }
  m_view->setTool(m_panel.tool());
  m_view->setFillMode(m_panel.fillMode(), m_panel.pickedColor());
  m_view->setBrushDiameter(m_panel.brushDiameter());
  m_view->setSelectionParams(m_panel.selectionParams());
}

bool RetouchController::save() {
  if ((m_state != EDITING) || !m_view) {
    return false;
  }
  const std::vector<Edit> edits(m_view->edits());
  if (edits.empty()) {
    return true;
  }
  if (m_target.output) {
    return saveOutput(edits);
  }
  const RetouchTarget target(m_target);
  const bool carry = m_carryOver;
  const double sx = m_carryX;
  const double sy = m_carryY;
  const QString path(target.imageId.filePath());
  const QString projectPath(target.projectImageId.filePath());
  const QString name(displayName());
  const QString projectName(displayNameOf(target.projectImageId));

  QStringList changedElsewhere;
  if (changedSince(path, m_loadedSize, m_loadedTime)) {
    changedElsewhere << name;
  }
  if (carry && changedSince(projectPath, m_projectLoadedSize, m_projectLoadedTime)) {
    changedElsewhere << projectName;
  }
  if (!changedElsewhere.isEmpty()) {
    const QMessageBox::StandardButton answer
        = QMessageBox::warning(m_window, tr("Retouch"),
                               tr("%1 changed on disk since it was opened here, possibly by someone else.\n\n"
                                  "Save over it anyway? The other changes would be lost.")
                                   .arg(changedElsewhere.join(QLatin1String(", "))),
                               QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Save) {
      return false;
    }
  }

  const QString outputDir(m_host.retouchOutputDirectory());
  const std::shared_ptr<ThumbnailPixmapCache> thumbnails(m_host.retouchThumbnailCache());
  // Thumbnails are of the project's pages, at the project's resolution.
  const Dpi projectDpi(m_page.metadata().dpi());
  const int paperRing = std::max(4, qRound(m_view->paperRing() * std::sqrt(sx * sy)));
  QString error;
  bool inputWritten = false;
  bool projectWritten = false;
  const bool ok = runWhileWaiting(tr("Saving %1...").arg(name), [&]() {
    try {
      // Both originals are kept before either file is touched.
      OriginalsBackup backup(outputDir);
      if (!backup.keep(path, &error) || (carry && !backup.keep(projectPath, &error))) {
        error = tr("The original could not be kept, so nothing was changed.\n\n%1").arg(error);
        return false;
      }
      QImage written;
      if (!SourceFile::rewrite(target.imageId, edits, &written, &error)) {
        return false;
      }
      inputWritten = true;
      if (target.editsProjectImage()) {
        if (thumbnails && !written.isNull()) {
          thumbnails->recreateThumbnail(target.projectImageId, displayForm(written, projectDpi));
        }
        return true;
      }
      if (!carry) {
        return true;
      }

      // The same edits, on the project's copy: scaled to it, and with paper
      // colours taken from its own paper.
      const QImage projectImage(ImageLoader::load(target.projectImageId));
      if (projectImage.isNull()) {
        error = tr("The project's copy, %1, could not be read.").arg(projectName);
        return false;
      }
      QImage projectWrittenImage;
      if (!SourceFile::rewrite(target.projectImageId, carryOver(edits, sx, sy, projectImage, paperRing),
                               &projectWrittenImage, &error)) {
        return false;
      }
      projectWritten = true;
      if (thumbnails && !projectWrittenImage.isNull()) {
        thumbnails->recreateThumbnail(target.projectImageId, displayForm(projectWrittenImage, projectDpi));
      }
      return true;
    } catch (const std::bad_alloc&) {
      error = tr("There is not enough memory to save it.");
    } catch (const std::exception& e) {
      error = QString::fromLocal8Bit(e.what());
    }
    return false;
  });

  if (!ok && !inputWritten) {
    core::CrashHandler::log(QStringLiteral("Retouch: saving %1 failed: %2").arg(path, error));
    QMessageBox::warning(m_window, tr("Retouch"), tr("%1 could not be saved.\n\n%2").arg(name, error));
    return false;
  }
  if (!ok) {
    // The input has the changes and the project's copy does not. Not something
    // to keep editing over: the input is saved, so the session ends, and the
    // operator is told plainly what is left to do.
    core::CrashHandler::log(
        QStringLiteral("Retouch: %1 was saved but the project's copy %2 was not: %3").arg(path, projectPath, error));
    QMessageBox::warning(m_window, tr("Retouch"),
                         tr("%1 was saved, but the project's copy %2 could not be changed.\n\n%3\n\n"
                            "The output will keep what was painted over until the project's copy is fixed.")
                             .arg(name, projectName, error));
  } else {
    core::CrashHandler::log(QStringLiteral("Retouch: wrote %1 change(s) into %2%3")
                                .arg(edits.size())
                                .arg(path, projectWritten ? QStringLiteral(" and %1").arg(projectPath) : QString()));
  }
  reset();
  if (target.editsProjectImage() || projectWritten) {
    m_host.retouchSourceReplaced(target.projectImageId);
  }
  updatePanel();
  pageChanged();
  showStatus(m_window,
             projectWritten ? tr("Saved %1 and the project's copy. Run the steps again to update the output.").arg(name)
                            : tr("Saved %1. The original is kept in the project's output folder.").arg(name),
             10000);
  return true;
}

bool RetouchController::saveOutput(const std::vector<Edit>& edits) {
  const QString name(displayName());
  QString error;
  if (!m_host.retouchOutputSaved(m_page, edits, &error)) {
    core::CrashHandler::log(QStringLiteral("Retouch: keeping %1 change(s) to the output of %2 failed: %3")
                                .arg(edits.size())
                                .arg(m_page.imageId().filePath(), error));
    QMessageBox::warning(m_window, tr("Retouch"), tr("The changes to %1 could not be kept.\n\n%2").arg(name, error));
    return false;
  }
  core::CrashHandler::log(QStringLiteral("Retouch: kept %1 change(s) to the output of %2 with the project")
                              .arg(edits.size())
                              .arg(m_page.imageId().filePath()));
  reset();
  updatePanel();
  pageChanged();
  showStatus(m_window,
             tr("The changes are kept with the project and painted over the output, now and whenever it is made "
                "again. Save the project to keep them."),
             12000);
  return true;
}

void RetouchController::closeRequested() {
  if (m_state == LOADING) {
    reset();
    updatePanel();
    return;
  }
  if (m_state != EDITING) {
    return;
  }
  if (hasUnsavedEdits()) {
    const int count = m_view->editCount();
    const QMessageBox::StandardButton answer = QMessageBox::question(
        m_window, tr("Retouch"), tr("Discard the %n unsaved change(s) to %1?", nullptr, count).arg(displayName()),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
      return;
    }
  }
  reset();
  updatePanel();
  m_host.retouchSessionEnded();
}

bool RetouchController::finish() {
  if (m_state == IDLE) {
    return true;
  }
  if (m_state == LOADING) {
    reset();
    updatePanel();
    return true;
  }
  if (hasUnsavedEdits()) {
    const int count = m_view->editCount();
    QMessageBox box(QMessageBox::Question, tr("Retouch"),
                    tr("%1 has %n unsaved change(s).", nullptr, count).arg(displayName()), QMessageBox::NoButton,
                    m_window);
    box.setInformativeText(m_target.output
                               ? tr("Save keeps them with the project and paints them over the page's output.")
                               : tr("Save writes them into the image file; a copy of the original is kept."));
    QPushButton* saveButton = box.addButton(tr("Save"), QMessageBox::AcceptRole);
    QPushButton* discardButton = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    box.addButton(tr("Keep Editing"), QMessageBox::RejectRole);
    box.setDefaultButton(saveButton);
    box.exec();
    if (box.clickedButton() == saveButton) {
      // On failure the session stays open, and so do the changes.
      return save();
    }
    if (box.clickedButton() != discardButton) {
      return false;
    }
  }
  reset();
  updatePanel();
  return true;
}

bool RetouchController::saveForProjectSave() {
  if (!hasUnsavedEdits()) {
    return true;
  }
  if (!save()) {
    return false;
  }
  m_host.retouchSessionEnded();
  return true;
}

void RetouchController::restoreRequested() {
  const PageInfo page(isActive() ? m_page : m_host.retouchCurrentPage());
  if (page.isNull()) {
    return;
  }
  RetouchTarget target(m_target);
  if (!isActive()) {
    QString whyNot;
    if (!m_host.retouchTarget(page, &target, &whyNot)) {
      QMessageBox::information(m_window, tr("Restore Original"), whyNot);
      return;
    }
  }
  if (target.output) {
    restoreOutput(page);
    return;
  }
  const QString path(target.imageId.filePath());
  const bool separateCopy = !target.projectImageId.isNull() && !target.editsProjectImage();
  const QString projectPath(target.projectImageId.filePath());
  const QString outputDir(m_host.retouchOutputDirectory());
  OriginalsBackup backup(outputDir);
  const bool restoreInput = backup.contains(path);
  const bool restoreProject = separateCopy && backup.contains(projectPath);
  if (!restoreInput && !restoreProject) {
    pageChanged();
    return;
  }
  const QString name(displayNameOf(target.imageId));
  QString question(restoreProject && restoreInput
                       ? tr("Put back the original of %1, and of the project's copy of it, as they were before "
                            "they were first retouched?")
                             .arg(name)
                       : tr("Put back the original of %1, as it was before it was first retouched?")
                             .arg(restoreInput ? name : displayNameOf(target.projectImageId)));
  if (hasUnsavedEdits()) {
    question += QLatin1String("\n\n") + tr("The unsaved changes will be discarded.");
  }
  if (QMessageBox::question(m_window, tr("Restore Original"), question, QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel)
      != QMessageBox::Yes) {
    return;
  }

  const std::shared_ptr<ThumbnailPixmapCache> thumbnails(m_host.retouchThumbnailCache());
  const Dpi projectDpi(page.metadata().dpi());
  const bool projectTouched = restoreProject || (restoreInput && target.editsProjectImage());
  QString error;
  const bool ok = runWhileWaiting(tr("Restoring %1...").arg(name), [&]() {
    try {
      if (restoreInput && !backup.restore(path, &error)) {
        return false;
      }
      if (restoreProject && !backup.restore(projectPath, &error)) {
        return false;
      }
      if (thumbnails && projectTouched) {
        const QImage image(ImageLoader::load(target.projectImageId));
        if (!image.isNull()) {
          thumbnails->recreateThumbnail(target.projectImageId, displayForm(image, projectDpi));
        }
      }
      return true;
    } catch (const std::bad_alloc&) {
      error = tr("There is not enough memory.");
    } catch (const std::exception& e) {
      error = QString::fromLocal8Bit(e.what());
    }
    return false;
  });
  if (!ok) {
    core::CrashHandler::log(QStringLiteral("Retouch: restoring %1 failed: %2").arg(path, error));
    QMessageBox::warning(m_window, tr("Restore Original"), tr("%1 could not be restored.\n\n%2").arg(name, error));
    return;
  }
  core::CrashHandler::log(QStringLiteral("Retouch: restored the original of %1%2")
                              .arg(restoreInput ? path : projectPath, (restoreInput && restoreProject)
                                                                          ? QStringLiteral(" and %1").arg(projectPath)
                                                                          : QString()));
  reset();
  if (projectTouched) {
    m_host.retouchSourceReplaced(target.projectImageId);
  }
  updatePanel();
  m_host.retouchSessionEnded();
}

void RetouchController::restoreOutput(const PageInfo& page) {
  if (!m_host.retouchOutputHasEdits(page)) {
    pageChanged();
    return;
  }
  QString question(tr("Remove all the retouching of the output of %1?").arg(displayNameOf(page.imageId())));
  if (hasUnsavedEdits()) {
    question += QLatin1String("\n\n") + tr("The unsaved changes will be discarded.");
  }
  if (QMessageBox::question(m_window, tr("Restore Original"), question, QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel)
      != QMessageBox::Yes) {
    return;
  }
  m_host.retouchOutputRestore(page);
  core::CrashHandler::log(QStringLiteral("Retouch: removed the retouching of the output of %1")
                              .arg(page.imageId().filePath()));
  reset();
  updatePanel();
  m_host.retouchSessionEnded();
}

void RetouchController::abandon() {
  if (m_state == IDLE) {
    return;
  }
  if (hasUnsavedEdits()) {
    core::CrashHandler::log(QStringLiteral("Retouch: %1 unsaved change(s) to %2 were dropped with the project")
                                .arg(m_view->editCount())
                                .arg(m_target.imageId.filePath()));
  }
  reset();
  updatePanel();
}

void RetouchController::setAvailable(const bool available, const QString& reason) {
  m_panel.setAvailable(available, reason);
  updatePanel();
  pageChanged();
}

void RetouchController::pageChanged() {
  if (m_state == LOADING) {
    return;
  }
  const QString outputDir(m_host.retouchOutputDirectory());
  bool backedUp = false;
  if (!outputDir.isEmpty() && m_host.retouchAllowed()) {
    RetouchTarget target(m_target);
    QString whyNot;
    const PageInfo page(m_host.retouchCurrentPage());
    if (isActive() || (!page.isNull() && m_host.retouchTarget(page, &target, &whyNot))) {
      if (target.output) {
        backedUp = m_host.retouchOutputHasEdits(isActive() ? m_page : page);
      } else {
        const OriginalsBackup backup(outputDir);
        backedUp = backup.contains(target.imageId.filePath())
                   || (!target.projectImageId.isNull() && !target.editsProjectImage()
                       && backup.contains(target.projectImageId.filePath()));
      }
    }
  }
  m_panel.setRestoreAvailable(backedUp);
}

void RetouchController::reset() {
  m_state = IDLE;
  ++m_generation;
  // The view belongs to the page area, which replaces it next.
  if (m_view) {
    m_view->disconnect(this);
  }
  m_view = nullptr;
  m_carryOver = false;
  m_carryNote.clear();
}

void RetouchController::updatePanel() {
  QString note;
  if (m_state == EDITING) {
    if (m_carryOver) {
      note = tr("Saving changes the project's copy (%1) too.").arg(displayNameOf(m_target.projectImageId));
    } else {
      note = m_carryNote;
    }
  }
  m_panel.setSessionState(m_state == LOADING, m_state == EDITING, displayName(), note);
  if (m_view && (m_state == EDITING)) {
    m_panel.setEditState(m_view->editCount(), m_view->canUndo(), m_view->canRedo());
    m_panel.setHasSelection(m_view->hasSelection());
  }
  updateEffectiveColor();
}

void RetouchController::updateEffectiveColor() {
  const QColor requested((m_panel.fillMode() == FillMode::PICKED) ? m_panel.pickedColor() : QColor(Qt::white));
  if (!m_view || (m_state != EDITING)) {
    m_panel.setEffectiveColor(requested, QString());
    return;
  }
  const QColor effective(m_view->effectiveColor(requested));
  QString note;
  if (effective.rgb() != requested.rgb()) {
    note = tr("This image can only hold %1 here.").arg(effective.name());
  }
  m_panel.setEffectiveColor(effective, note);
}

bool RetouchController::runWhileWaiting(const QString& message, const std::function<bool()>& work) {
  bool result = false;
  QThread* thread = QThread::create([&result, &work]() {
    core::diag::setThreadRole("retouch");
    result = work();
  });
  QEventLoop loop;
  connect(thread, &QThread::finished, &loop, &QEventLoop::quit);

  // Shown only if the work takes long enough to notice, so that a quick save
  // does not flash a dialog.
  QProgressDialog progress(message, QString(), 0, 0, m_window);
  progress.setWindowTitle(tr("Retouch"));
  progress.setWindowModality(Qt::WindowModal);
  progress.setCancelButton(nullptr);
  progress.setMinimumDuration(400);
  // Starts the countdown to showing it; without a value it waits its default four seconds.
  progress.setValue(0);

  QApplication::setOverrideCursor(Qt::WaitCursor);
  thread->start();
  // Keeps painting, but not taking clicks: nothing may change the project
  // while its source file is being replaced.
  loop.exec(QEventLoop::ExcludeUserInputEvents);
  thread->wait();
  delete thread;
  QApplication::restoreOverrideCursor();
  return result;
}
