// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_RETOUCHTYPES_H_
#define SCANTAILOR_APP_RETOUCHTYPES_H_

namespace retouch {
/** \brief What a click on the image does while retouching. */
enum class Tool {
  RECTANGLE, /**< Drag out a rectangle; it is filled. */
  BRUSH,     /**< Paint freehand with a round brush. */
  SELECT,    /**< Click an object - a stamp, a blot - to select it for filling. */
  PICK       /**< Click to take the fill colour from the image. */
};

/** \brief Which colour the tools paint with. */
enum class FillMode {
  WHITE,  /**< Plain white. */
  PICKED, /**< A colour picked from the image, or chosen in the colour dialog. */
  AUTO    /**< For each change, the colour of the paper just around it. */
};
}  // namespace retouch

#endif  // SCANTAILOR_APP_RETOUCHTYPES_H_
