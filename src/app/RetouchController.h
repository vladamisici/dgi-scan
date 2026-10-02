// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_RETOUCHCONTROLLER_H_
#define SCANTAILOR_APP_RETOUCHCONTROLLER_H_

#include <Edit.h>

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QTransform>
#include <functional>
#include <memory>
#include <vector>

#include "Dpi.h"
#include "ImageId.h"
#include "OrthogonalRotation.h"
#include "PageId.h"
#include "PageInfo.h"
#include "RetouchTypes.h"

class ImageViewBase;
class QImage;
class QWidget;
class RetouchPanel;
class RetouchView;
class ThumbnailPixmapCache;

/**
 * \brief The image a page is retouched on, and the file the changes go into.
 *
 * Usually the input image of the page: in compare mode the one shown on the
 * left; otherwise the project's own source image. At the Output stage it is
 * the page's output, painted on as it is shown, while the changes are carried
 * into the project's image through the transform the output was made with:
 * so that the steps run again on what is left - Select Content finding the
 * content without the stamp, and the margins and the output following.
 */
struct RetouchTarget {
  /** The image edited: shown, and written into unless it is the output. */
  ImageId imageId;
  /**
   * The project's own image of the page. Usually the same file; in compare
   * mode the input image may be a separate copy, and this one is changed
   * along with it, so that the output loses what the input did.
   */
  ImageId projectImageId;
  /** The size of projectImageId, as the project knows it. */
  QSize projectImageSize;
  /** The resolution to show the file at; null for the file's own. */
  Dpi dpi;
  OrthogonalRotation rotation;
  /** The header over the image while it is being edited. */
  QString title;
  /** Whether imageId is the page's output: painted on, never written. */
  bool output = false;
  /** For the output: from its pixels to those of the project's image - crop, skew, rotation, resolution. */
  QTransform outputToSource;

  /** \brief Whether the project's image is the very file being edited. */
  bool editsProjectImage() const;
};


/**
 * \brief What retouching needs from the main window.
 */
class RetouchHost {
 public:
  virtual ~RetouchHost() = default;

  /** \brief The page on screen, or a null PageInfo if there is none. */
  virtual PageInfo retouchCurrentPage() const = 0;

  /** \brief Whether a page can be retouched now: a project is open and no batch is running. */
  virtual bool retouchAllowed() const = 0;

  /**
   * \brief The file \p page is retouched in.
   *
   * \return false, with the reason in \p whyNot, if the page has none.
   */
  virtual bool retouchTarget(const PageInfo& page, RetouchTarget* target, QString* whyNot) const = 0;

  /** \brief Where the project's output goes; the originals are kept under it. */
  virtual QString retouchOutputDirectory() const = 0;

  virtual std::shared_ptr<ThumbnailPixmapCache> retouchThumbnailCache() const = 0;

  /**
   * \brief Puts \p editor where the image it edits is shown, taking ownership of it.
   *
   * \p output: the image is the page's output, rather than its input.
   */
  virtual void retouchShowEditor(QWidget* editor, const QString& title, bool output) = 0;

  /**
   * \brief The view showing the pixels of the project's image of the page, if one is on screen.
   *
   * Beside the editor, in verification mode; at a stage that shows the image
   * itself rather than the output made from it.
   */
  virtual ImageViewBase* retouchProjectView() const = 0;

  /** \brief Retouching is over: show the page the way the current stage does. */
  virtual void retouchSessionEnded() = 0;

  /** \brief The project's image \p projectImageId now holds different pixels. */
  virtual void retouchSourceReplaced(const ImageId& projectImageId) = 0;

  /**
   * \brief The view showing \p inputImage beside the output, if one is on screen.
   *
   * In compare mode, at the Output stage. \p sx and \p sy are set to how its
   * image's pixels relate to the file's: it may show a reduced copy.
   */
  virtual ImageViewBase* retouchInputView(const ImageId& inputImage, double* sx, double* sy) const = 0;

  /**
   * \brief Whether \p page's output has retouching kept with the project.
   *
   * Made by the version that painted on the output alone; its changes are put
   * back along with the input image's.
   */
  virtual bool retouchOutputHasEdits(const PageInfo& page) const = 0;

  /** \brief Removes that retouching; the output is made again without it. */
  virtual void retouchOutputRestore(const PageInfo& page) = 0;
};


/**
 * \brief Runs a retouching session: one page's source image, opened, edited, saved.
 *
 * A session starts when a tool is picked, loads the image in the background,
 * and shows it in place of the page. It ends when the operator saves or closes
 * it, or moves on to something else - another page, another stage, another
 * project - in which case unsaved changes are asked about first.
 *
 * Saving keeps a copy of the original file the first time a file is saved
 * over, writes the new one beside it and renames it into place, all on a
 * worker thread while the window waits: a scan on a network share can take
 * seconds to write.
 */
class RetouchController : public QObject {
  Q_OBJECT
 public:
  RetouchController(RetouchHost& host, RetouchPanel& panel, QWidget* window);

  ~RetouchController() override;

  /** \brief Whether a session is loading or editing. */
  bool isActive() const { return m_state != IDLE; }

  bool hasUnsavedEdits() const;

  /** \brief The page the session is editing. */
  const PageId& sessionPageId() const { return m_page.id(); }

  /**
   * \brief Ends the session before the page area shows something else.
   *
   * Unsaved changes are asked about: saved, discarded, or kept, the last by
   * returning false, in which case the caller must leave the page area alone.
   */
  bool finish();

  /**
   * \brief Saves unsaved changes and ends the session, for File > Save.
   *
   * \return false if the changes could not be saved, which has been reported.
   */
  bool saveForProjectSave();

  /**
   * \brief Ends the session at once, unsaved changes and all.
   *
   * For when the project is going away regardless - out of memory, say - and
   * the operator has had their say already, or cannot have it.
   */
  void abandon();

  /**
   * \brief Whether pages can be retouched at all, now: a project is open and no
   *        batch is running. \p reason says why not, if it is anything else.
   */
  void setAvailable(bool available, const QString& reason = QString());

  /** \brief Another page is on screen: the panel offers what applies to it. */
  void pageChanged();

 private:
  enum State { IDLE, LOADING, EDITING };

  struct LoadedImage;
  class LoadTask;
  class LoadResult;

  void toolActivated(retouch::Tool tool);

  void begin(retouch::Tool tool);

  void loaded(int generation, const std::shared_ptr<LoadedImage>& image);

  /** \return whether the file was written. Failures are reported. */
  bool save();

  /** \brief save() for edits painted on the output: carried into the project's image. */
  bool saveOutput(const std::vector<retouch::Edit>& edits);

  void closeRequested();

  void restoreRequested();

  /** \brief Forgets the session without telling the host. */
  void reset();

  void updatePanel();

  void updateEffectiveColor();

  void applyPanelSettings();

  /**
   * \brief Runs \p work on a worker thread, with the window held until it is done.
   *
   * \return what \p work returned.
   */
  bool runWhileWaiting(const QString& message, const std::function<bool()>& work);

  QString displayName() const;

  RetouchHost& m_host;
  RetouchPanel& m_panel;
  QPointer<QWidget> m_window;
  State m_state;
  /** Bumped by every session, so that a load finishing late can tell it is no longer wanted. */
  int m_generation;
  /** The page on screen when the session started. */
  PageInfo m_page;
  /** The file being edited. */
  RetouchTarget m_target;
  QPointer<RetouchView> m_view;
  /** The file's size and time when loaded, to notice it being changed by someone else meanwhile. */
  qint64 m_loadedSize;
  QDateTime m_loadedTime;
  /** Whether saving writes the edits into the project's separate copy too. */
  bool m_carryOver;
  /** How the edited image's pixels map onto those of the project's copy. */
  double m_carryX;
  double m_carryY;
  /** Why the project's copy will not be changed, when it will not. */
  QString m_carryNote;
  qint64 m_projectLoadedSize;
  QDateTime m_projectLoadedTime;
  retouch::Tool m_toolBeforePick;
};


#endif  // SCANTAILOR_APP_RETOUCHCONTROLLER_H_
