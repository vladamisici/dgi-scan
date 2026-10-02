// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_SELECT_CONTENT_SETTINGS_H_
#define SCANTAILOR_SELECT_CONTENT_SETTINGS_H_

#include <DeviationProvider.h>

#include <QMutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "NonCopyable.h"
#include "PageId.h"
#include "Params.h"

class AbstractRelinker;

namespace select_content {
class Settings {
  DECLARE_NON_COPYABLE(Settings)

 public:
  Settings();

  virtual ~Settings();

  void clear();

  void performRelinking(const AbstractRelinker& relinker);

  void setPageParams(const PageId& pageId, const Params& params);

  void clearPageParams(const PageId& pageId);

  std::unique_ptr<Params> getPageParams(const PageId& pageId) const;

  bool isParamsNull(const PageId& pageId) const;

  QSizeF pageDetectionBox() const;

  void setPageDetectionBox(QSizeF size);

  double pageDetectionTolerance() const;

  void setPageDetectionTolerance(double tolerance);

  const DeviationProvider<PageId>& deviationProvider() const;

  /**
   * \brief Asks for the page's content box to be found again, the next time
   *        the page is processed, whatever its mode.
   *
   * For when its image's pixels changed - a stamp painted out. Its mode is
   * kept: a page whose box was found once and then frozen ("disabled"), or set
   * by hand, gets the box found in what is left, frozen or manual in turn. Not
   * saved with the project.
   */
  void requestRedetection(const PageId& pageId);

  bool isRedetectionRequested(const PageId& pageId) const;

  /** \brief The request has been met: the box found has been stored. */
  void clearRedetection(const PageId& pageId);

 private:
  using PageParams = std::unordered_map<PageId, Params>;

  mutable QMutex m_mutex;
  PageParams m_pageParams;
  std::unordered_set<PageId> m_redetectionRequests;
  QSizeF m_pageDetectionBox;
  double m_pageDetectionTolerance;
  DeviationProvider<PageId> m_deviationProvider;
};
}  // namespace select_content
#endif  // ifndef SCANTAILOR_SELECT_CONTENT_SETTINGS_H_
