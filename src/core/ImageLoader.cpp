// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "ImageLoader.h"

#include <QFile>
#include <QImage>
#include <QtGui/QImageReader>

#include "Diagnostics.h"
#include "ImageId.h"
#include "TiffReader.h"

QImage ImageLoader::load(const ImageId& imageId) {
  return load(imageId.filePath(), imageId.zeroBasedPage());
}

QImage ImageLoader::load(const QString& filePath, const int pageNum) {
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    return QImage();
  }
  return load(file, pageNum);
}

QImage ImageLoader::load(QIODevice& ioDev, const int pageNum) {
  // Timed here rather than in the path overloads, because the output stage's
  // cached-result loads come straight to this one with a file they opened.
  DIAG_SCOPE(diagScope, "image.load");
  // The size goes with the time: decoding cost scales with pixels, so a slow
  // load of a huge scan and a slow load of a small one point at different causes.
  const auto finish = [&diagScope](QImage loaded) {
    diagScope.attr(core::diag::Attr("w", loaded.width()));
    diagScope.attr(core::diag::Attr("h", loaded.height()));
    diagScope.attr(core::diag::Attr("mb", loaded.sizeInBytes() / 1048576.0));
    return loaded;
  };

  if (TiffReader::canRead(ioDev)) {
    return finish(TiffReader::readImage(ioDev, pageNum));
  }

  if (pageNum != 0) {
    // Qt can only load the first page of multi-page images.
    return finish(QImage());
  }

  QImage image;
  QImageReader(&ioDev).read(&image);
  return finish(image);
}
