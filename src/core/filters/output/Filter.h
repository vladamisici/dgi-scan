// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_OUTPUT_FILTER_H_
#define SCANTAILOR_OUTPUT_FILTER_H_

#include <Edit.h>

#include <QCoreApplication>
#include <QImage>
#include <memory>
#include <vector>

#include "AbstractFilter.h"
#include "FillZonePropFactory.h"
#include "FilterResult.h"
#include "NonCopyable.h"
#include "PageView.h"
#include "PictureZonePropFactory.h"
#include "SafeDeletingQObjectPtr.h"

class ImageId;
class PageSelectionAccessor;
class ThumbnailPixmapCache;
class OutputFileNameGenerator;
class QString;

namespace output {
class OptionsWidget;
class Task;
class CacheDrivenTask;
class Settings;

class Filter : public AbstractFilter {
  DECLARE_NON_COPYABLE(Filter)

  Q_DECLARE_TR_FUNCTIONS(output::Filter)
 public:
  explicit Filter(const PageSelectionAccessor& pageSelectionAccessor);

  ~Filter() override;

  QString getName() const override;

  PageView getView() const override;

  void performRelinking(const AbstractRelinker& relinker) override;

  void preUpdateUI(FilterUiInterface* ui, const PageInfo& pageInfo) override;

  QDomElement saveSettings(const ProjectWriter& writer, QDomDocument& doc) const override;

  void loadSettings(const ProjectReader& reader, const QDomElement& filtersEl) override;

  void loadDefaultSettings(const PageInfo& pageInfo) override;

  /**
   * \brief Makes every page cut from \p imageId produce its output afresh.
   *
   * For when the image file itself has been changed. The output is otherwise
   * regenerated only when the file's size changes, and painting over part of
   * an uncompressed scan leaves the size exactly as it was.
   */
  void invalidateOutputFor(const ImageId& imageId);

  /**
   * \brief Adds \p edits, painted on the page's output as it is now, to the page's retouching.
   *
   * The output is made again, with them, the next time the page is processed;
   * every time after that too. \see retouch::OutputLayer
   *
   * \return false, with the reason in \p error, if the page's output has not
   *         been made in this session, so the edits could not be placed on it.
   */
  bool addRetouch(const PageId& pageId, const std::vector<retouch::Edit>& edits, QString* error);

  bool hasRetouch(const PageId& pageId) const;

  /** \brief Removes the page's retouching; its output is made again without it. */
  void clearRetouch(const PageId& pageId);

  std::shared_ptr<Task> createTask(const PageId& pageId,
                                   std::shared_ptr<ThumbnailPixmapCache> thumbnailCache,
                                   const OutputFileNameGenerator& outFileNameGen,
                                   bool batch,
                                   bool debug);

  std::shared_ptr<CacheDrivenTask> createCacheDrivenTask(const OutputFileNameGenerator& outFileNameGen);

  OptionsWidget* optionsWidget();

  std::vector<PageOrderOption> pageOrderOptions() const override;

  int selectedPageOrder() const override;

  void selectPageOrder(int option) override;

 private:
  void writePageSettings(QDomDocument& doc, QDomElement& filterEl, const PageId& pageId, int numericId) const;

  std::shared_ptr<Settings> m_settings;
  SafeDeletingQObjectPtr<OptionsWidget> m_optionsWidget;
  PictureZonePropFactory m_pictureZonePropFactory;
  FillZonePropFactory m_fillZonePropFactory;
  std::vector<PageOrderOption> m_pageOrderOptions;
  int m_selectedPageOrder;
};
}  // namespace output
#endif  // ifndef SCANTAILOR_OUTPUT_FILTER_H_
