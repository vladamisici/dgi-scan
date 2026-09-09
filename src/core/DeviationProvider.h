// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_DEVIATIONPROVIDER_H_
#define SCANTAILOR_CORE_DEVIATIONPROVIDER_H_

#include <foundation/NonCopyable.h>

#include <QMutex>
#include <QMutexLocker>
#include <cmath>
#include <functional>
#include <unordered_map>

/**
 * \brief Tracks per-page values and tells which of them deviate from the mean.
 *
 * Internally synchronised. Filter Settings objects update it from worker threads
 * as pages finish processing, while the GUI thread reads it to decorate and sort
 * thumbnails. Without a lock those two touch the same std::unordered_map
 * concurrently, and a rehash triggered by an insert while the GUI thread is
 * walking a bucket is a straightforward crash - one that gets more likely the
 * more pages a project has. Locking here changes no computed value.
 */
template <typename K, typename Hash = std::hash<K>>
class DeviationProvider {
  DECLARE_NON_COPYABLE(DeviationProvider)
 public:
  DeviationProvider() = default;

  explicit DeviationProvider(const std::function<double(const K&)>& computeValueByKey);

  bool isDeviant(const K& key, double coefficient = 1.0, double threshold = 0.0, bool defaultVal = false) const;

  double getDeviationValue(const K& key) const;

  void addOrUpdate(const K& key);

  void addOrUpdate(const K& key, double value);

  void remove(const K& key);

  void clear();

  void setComputeValueByKey(const std::function<double(const K&)>& computeValueByKey);

 protected:
  /** \brief Recomputes the cached mean and deviation. Caller must hold m_mutex. */
  void update() const;

 private:
  mutable QMutex m_mutex;
  std::function<double(const K&)> m_computeValueByKey;
  std::unordered_map<K, double, Hash> m_keyValueMap;

  // Cached values.
  mutable bool m_needUpdate = false;
  mutable double m_meanValue = 0.0;
  mutable double m_standardDeviation = 0.0;
};


template <typename K, typename Hash>
DeviationProvider<K, Hash>::DeviationProvider(const std::function<double(const K&)>& computeValueByKey)
    : m_computeValueByKey(computeValueByKey) {}

template <typename K, typename Hash>
bool DeviationProvider<K, Hash>::isDeviant(const K& key, double coefficient, double threshold, bool defaultVal) const {
  const QMutexLocker locker(&m_mutex);

  if (m_keyValueMap.find(key) == m_keyValueMap.end()) {
    return false;
  }
  if (m_keyValueMap.size() < 3) {
    return false;
  }

  double value = m_keyValueMap.at(key);
  if (std::isnan(value)) {
    return defaultVal;
  }

  update();
  return (std::abs(value - m_meanValue)
          > std::max((coefficient * m_standardDeviation), (threshold / 100) * m_meanValue));
}

template <typename K, typename Hash>
double DeviationProvider<K, Hash>::getDeviationValue(const K& key) const {
  const QMutexLocker locker(&m_mutex);

  if (m_keyValueMap.find(key) == m_keyValueMap.end()) {
    return -1.0;
  }
  if (m_keyValueMap.size() < 2) {
    return .0;
  }

  double value = m_keyValueMap.at(key);
  if (std::isnan(value)) {
    return -1.0;
  }

  update();
  return std::abs(m_keyValueMap.at(key) - m_meanValue);
}

template <typename K, typename Hash>
void DeviationProvider<K, Hash>::addOrUpdate(const K& key) {
  std::function<double(const K&)> computeValueByKey;
  {
    const QMutexLocker locker(&m_mutex);
    computeValueByKey = m_computeValueByKey;
  }

  // Invoked outside the lock on purpose: the callback reaches back into the
  // owning Settings object, which holds its own mutex, so calling it while
  // holding ours would invert the lock order its callers already establish.
  const double value = computeValueByKey(key);

  const QMutexLocker locker(&m_mutex);
  m_needUpdate = true;
  m_keyValueMap[key] = value;
}

template <typename K, typename Hash>
void DeviationProvider<K, Hash>::addOrUpdate(const K& key, const double value) {
  const QMutexLocker locker(&m_mutex);

  m_needUpdate = true;
  m_keyValueMap[key] = value;
}

template <typename K, typename Hash>
void DeviationProvider<K, Hash>::remove(const K& key) {
  const QMutexLocker locker(&m_mutex);

  m_needUpdate = true;

  if (m_keyValueMap.find(key) == m_keyValueMap.end()) {
    return;
  }
  m_keyValueMap.erase(key);
}

template <typename K, typename Hash>
void DeviationProvider<K, Hash>::update() const {
  if (!m_needUpdate) {
    return;
  }
  if (m_keyValueMap.size() < 2) {
    return;
  }

  int count = 0;
  {
    double sum = .0;
    for (const auto& [key, value] : m_keyValueMap) {
      if (!std::isnan(value)) {
        sum += value;
        count++;
      }
    }
    m_meanValue = sum / count;
  }

  {
    double differencesSum = .0;
    for (const auto& [key, value] : m_keyValueMap) {
      if (!std::isnan(value)) {
        differencesSum += std::pow(value - m_meanValue, 2);
      }
    }
    m_standardDeviation = std::sqrt(differencesSum / (count - 1));
  }

  m_needUpdate = false;
}

template <typename K, typename Hash>
void DeviationProvider<K, Hash>::setComputeValueByKey(const std::function<double(const K&)>& computeValueByKey) {
  const QMutexLocker locker(&m_mutex);

  this->m_computeValueByKey = computeValueByKey;
}

template <typename K, typename Hash>
void DeviationProvider<K, Hash>::clear() {
  const QMutexLocker locker(&m_mutex);

  m_keyValueMap.clear();

  m_needUpdate = false;
  m_meanValue = 0.0;
  m_standardDeviation = 0.0;
}


#endif  // SCANTAILOR_CORE_DEVIATIONPROVIDER_H_
