// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_APPUPDATE_H_
#define SCANTAILOR_CORE_APPUPDATE_H_

#include <QString>
#include <tuple>

namespace core {
/**
 * \brief A release: 1.0.16.12 for 1.0.16-dgi.12.
 *
 * The installer carries it as its product version, which is how an installer
 * found on the server is told apart from the one that installed this copy: the
 * file name stays the same from one dgi release to the next.
 */
struct ReleaseVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
  int build = 0;

  bool isNull() const { return (major == 0) && (minor == 0) && (patch == 0) && (build == 0); }

  /** \brief As releases are named: "1.0.16-dgi.12". */
  QString toString() const;

  /** \brief The inverse of toString(); a null version if \p text is not one. */
  static ReleaseVersion fromString(const QString& text);

  friend bool operator<(const ReleaseVersion& lhs, const ReleaseVersion& rhs) {
    return std::tie(lhs.major, lhs.minor, lhs.patch, lhs.build) < std::tie(rhs.major, rhs.minor, rhs.patch, rhs.build);
  }

  friend bool operator==(const ReleaseVersion& lhs, const ReleaseVersion& rhs) {
    return std::tie(lhs.major, lhs.minor, lhs.patch, lhs.build) == std::tie(rhs.major, rhs.minor, rhs.patch, rhs.build);
  }

  friend bool operator!=(const ReleaseVersion& lhs, const ReleaseVersion& rhs) { return !(lhs == rhs); }
};


/**
 * \brief Finding a newer installer of the application where the office keeps them.
 */
class AppUpdate {
 public:
  struct Installer {
    QString path;
    ReleaseVersion version;

    bool isNull() const { return path.isEmpty(); }
  };

  /** \brief The release this build of the application is. */
  static ReleaseVersion current();

  /**
   * \brief The product version in the file properties of the program \p path.
   *
   * \return A null version for a file without one, or that cannot be read -
   *         installers made before releases carried their version, for one.
   */
  static ReleaseVersion productVersion(const QString& path);

  /**
   * \brief The newest installer at \p location.
   *
   * \p location is an installer itself, or a folder, in which every
   * scantailor-dgi-*-win64.exe counts. Files without a version are passed over.
   * Touches the network: not for the GUI thread.
   *
   * \return The installer, or a null one if there is none.
   */
  static Installer newestInstaller(const QString& location);
};
}  // namespace core

#endif  // SCANTAILOR_CORE_APPUPDATE_H_
