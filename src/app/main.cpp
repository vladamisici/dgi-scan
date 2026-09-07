// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <config.h>
#include <core/Application.h>
#include <core/ApplicationSettings.h>
#include <core/ColorSchemeFactory.h>
#include <core/ColorSchemeManager.h>
#include <core/CrashHandler.h>
#include <core/FontIconPack.h>
#include <core/IconProvider.h>
#include <core/StyledIconPack.h>

#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
  QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

  Application app(argc, argv);

#ifdef _WIN32
  // Get rid of all references to Qt's installation directory.
  Application::setLibraryPaths(QStringList(Application::applicationDirPath()));
#endif

  QStringList args = Application::arguments();

  // This information is used by QSettings.
  Application::setApplicationName(APPLICATION_NAME);
  Application::setOrganizationName(ORGANIZATION_NAME);

  QSettings::setDefaultFormat(QSettings::IniFormat);
  if (app.isPortableVersion()) {
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, app.getPortableConfigPath());
  }
  QSettings settings;

  // Installed before any project is touched. Until now a crash - most often an
  // exception escaping a worker thread - killed the process without a dialog,
  // without a log line and without a dump, which is why "it just closes" reports
  // were impossible to act on. From here on every such death leaves a minidump
  // and a log entry behind.
  core::CrashHandler::install(app.isPortableVersion()
                                  ? app.getPortableConfigPath() + QLatin1String("/crashes")
                                  : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                        + QLatin1String("/crashes"));

  app.installLanguage(ApplicationSettings::getInstance().getLanguage());

  {
    std::unique_ptr<ColorScheme> scheme
        = ColorSchemeFactory().create(ApplicationSettings::getInstance().getColorScheme());
    ColorSchemeManager::instance().setColorScheme(*scheme);
  }
  IconProvider::getInstance().setIconPack(StyledIconPack::createDefault());

  auto* mainWnd = new MainWindow();
  mainWnd->setAttribute(Qt::WA_DeleteOnClose);
  if (settings.value("mainWindow/maximized") == false) {
    mainWnd->show();
  } else {
    // mainWnd->showMaximized();  // Doesn't work for Windows.
    QTimer::singleShot(0, mainWnd, &QMainWindow::showMaximized);
  }

  if (args.size() > 1) {
    mainWnd->openProject(args.at(1));
  }

  // After any project named on the command line has been opened, so that a
  // recovery offer never competes with an explicit request.
  mainWnd->offerUnsavedSessionRecovery();

  return Application::exec();
}  // main
