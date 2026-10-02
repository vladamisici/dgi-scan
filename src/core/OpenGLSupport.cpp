// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "OpenGLSupport.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSettings>
#include <QSurfaceFormat>

bool OpenGLSupport::supported() {
  // Memoised. This used to build and tear down a real QOpenGLContext on every
  // ImageViewBase construction - that is, on every page and every stage change.
  // The answer cannot change during a run, and repeatedly creating contexts
  // against a remote-desktop or software GL stack is both slow and a needless
  // source of driver-level failures. Thread-safe initialisation is guaranteed by
  // the C++ rules for function-local statics.
  static const bool isSupported = [] {
    QSurfaceFormat format;
    format.setSamples(2);
    format.setAlphaBufferSize(8);

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
      return false;
    }
    format = context.format();

    if (format.samples() < 2) {
      return false;
    }
    return format.hasAlpha();
  }();
  return isSupported;
}

QString OpenGLSupport::deviceName() {
  QString name;
  QOpenGLContext context;
  QOffscreenSurface surface;
  if (context.create() && (surface.create(), true) && context.makeCurrent(&surface)) {
    name = QString::fromUtf8((const char*) context.functions()->glGetString(GL_RENDERER));
    context.doneCurrent();
  }
  return name;
}
