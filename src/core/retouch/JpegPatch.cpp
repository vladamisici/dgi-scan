// Copyright (C) 2026  DGI
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "JpegPatch.h"

#include <QCoreApplication>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace retouch {
namespace {
struct Tr {
  Q_DECLARE_TR_FUNCTIONS(retouch::JpegPatch)
};

/**
 * libjpeg reports errors by calling error_exit, which must not return. The
 * usual answer is a longjmp back to the caller, which is safe here because
 * everything that needs cleaning up lives in a Session allocated before the
 * setjmp() and destroyed normally after it.
 */
struct ErrorManager {
  jpeg_error_mgr pub;  // Must come first: libjpeg sees only this part.
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void errorExit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

void ignoreMessage(j_common_ptr) {}

/** Everything libjpeg allocates, released however the work ends. */
struct Session {
  ErrorManager err{};
  jpeg_decompress_struct src{};
  jpeg_compress_struct dst{};
  bool srcCreated = false;
  bool dstCreated = false;
  unsigned char* outBuffer = nullptr;
  unsigned long outSize = 0;

  Session() {
    jpeg_std_error(&err.pub);
    err.pub.error_exit = &errorExit;
    // Warnings - a corrupt-looking but decodable marker, for one - are not
    // failures, and there is nowhere to show them.
    err.pub.output_message = &ignoreMessage;
    err.message[0] = '\0';
    src.err = &err.pub;
    dst.err = &err.pub;
  }

  ~Session() {
    if (dstCreated) {
      jpeg_destroy_compress(&dst);
    }
    if (srcCreated) {
      jpeg_destroy_decompress(&src);
    }
    // jpeg_mem_dest() allocates with malloc().
    std::free(outBuffer);
  }

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
};

/** Why the header just read cannot be patched, or an empty string. */
QString headerUnsupportedReason(const jpeg_decompress_struct& info) {
  if (info.data_precision != 8) {
    return Tr::tr("it is a %1-bit JPEG").arg(info.data_precision);
  }
  const bool gray = (info.num_components == 1) && (info.jpeg_color_space == JCS_GRAYSCALE);
  const bool color
      = (info.num_components == 3) && ((info.jpeg_color_space == JCS_YCbCr) || (info.jpeg_color_space == JCS_RGB));
  if (!gray && !color) {
    return Tr::tr("its colour space (CMYK or similar) is not supported");
  }
  for (int ci = 0; ci < info.num_components; ++ci) {
    const jpeg_component_info& comp = info.comp_info[ci];
    if ((comp.h_samp_factor <= 0) || (comp.v_samp_factor <= 0) || (info.max_h_samp_factor % comp.h_samp_factor != 0)
        || (info.max_v_samp_factor % comp.v_samp_factor != 0)) {
      return Tr::tr("its chroma subsampling is not supported");
    }
  }
  return QString();
}

void startReading(Session& session, const QByteArray& data) {
  jpeg_create_decompress(&session.src);
  session.srcCreated = true;
  // Older libjpeg versions take a non-const buffer, though they never write to it.
  jpeg_mem_src(&session.src, reinterpret_cast<unsigned char*>(const_cast<char*>(data.constData())),
               static_cast<unsigned long>(data.size()));
}

/**
 * Marks every 8x8 cell of pixels in which \p edited differs from \p original.
 * Cell (cx, cy) holds pixels [8cx, 8cx + 8) x [8cy, 8cy + 8).
 */
bool markChangedCells(const QImage& original,
                      const QImage& edited,
                      const QRect& area,
                      const int cellsPerRow,
                      std::vector<unsigned char>& cells) {
  bool any = false;
  const int depth = original.depth();
  for (int y = area.top(); y <= area.bottom(); ++y) {
    const uchar* a = original.constScanLine(y);
    const uchar* b = edited.constScanLine(y);
    unsigned char* rowCells = &cells[size_t(y / 8) * cellsPerRow];
    for (int x = area.left(); x <= area.right(); ++x) {
      bool differs;
      if (depth == 32) {
        differs = reinterpret_cast<const quint32*>(a)[x] != reinterpret_cast<const quint32*>(b)[x];
      } else if (depth == 8) {
        differs = a[x] != b[x];
      } else {
        differs = original.pixel(x, y) != edited.pixel(x, y);
      }
      if (differs) {
        rowCells[x / 8] = 1;
        any = true;
      }
    }
  }
  return any;
}

/** cosTable[x][u] = C(u) cos((2x + 1) u pi / 16), with C(0) = 1 / sqrt(2) and C(u) = 1 otherwise. */
struct CosTable {
  double value[8][8];

  CosTable() {
    const double pi = 3.14159265358979323846;
    for (int x = 0; x < 8; ++x) {
      for (int u = 0; u < 8; ++u) {
        value[x][u] = ((u == 0) ? std::sqrt(0.5) : 1.0) * std::cos((2 * x + 1) * u * pi / 16.0);
      }
    }
  }
};

/**
 * The forward DCT of one block of level-shifted samples, quantized the way
 * libjpeg quantizes: F(u, v) divided by the quantizer and rounded half away
 * from zero. Coefficients come out in natural (row-major) order, which is the
 * order both JBLOCK and JQUANT_TBL use.
 */
void forwardDctQuantize(const double samples[8][8], const UINT16* quantval, JCOEF* out) {
  static const CosTable table;
  double rows[8][8];  // [y][u]
  for (int y = 0; y < 8; ++y) {
    for (int u = 0; u < 8; ++u) {
      double sum = 0.0;
      for (int x = 0; x < 8; ++x) {
        sum += samples[y][x] * table.value[x][u];
      }
      rows[y][u] = sum;
    }
  }
  for (int v = 0; v < 8; ++v) {
    for (int u = 0; u < 8; ++u) {
      double sum = 0.0;
      for (int y = 0; y < 8; ++y) {
        sum += rows[y][u] * table.value[y][v];
      }
      const double quantized = 0.25 * sum / quantval[v * 8 + u];
      const double rounded = (quantized >= 0.0) ? std::floor(quantized + 0.5) : -std::floor(-quantized + 0.5);
      out[v * 8 + u] = static_cast<JCOEF>(std::max(-32768.0, std::min(32767.0, rounded)));
    }
  }
}

/** The value of component \p component of a pixel, as libjpeg's colour conversion computes it. */
double componentValue(const QRgb rgb, const J_COLOR_SPACE colorSpace, const int component) {
  const double r = qRed(rgb);
  const double g = qGreen(rgb);
  const double b = qBlue(rgb);
  switch (colorSpace) {
    case JCS_YCbCr:
      switch (component) {
        case 0:
          return 0.29900 * r + 0.58700 * g + 0.11400 * b;
        case 1:
          return -0.16874 * r - 0.33126 * g + 0.50000 * b + 128.0;
        default:
          return 0.50000 * r - 0.41869 * g - 0.08131 * b + 128.0;
      }
    case JCS_RGB:
      return (component == 0) ? r : ((component == 1) ? g : b);
    default:
      // Grayscale: the image is grey already.
      return qGray(rgb);
  }
}

/**
 * Re-encodes the blocks of every component that cover a changed cell, from the
 * pixels of \p edited.
 */
void patchBlocks(jpeg_decompress_struct& info,
                 jvirt_barray_ptr* coefArrays,
                 const QImage& edited,
                 const std::vector<unsigned char>& changedCells,
                 const int cellsPerRow,
                 const int cellRows) {
  const int width = edited.width();
  const int height = edited.height();

  for (int ci = 0; ci < info.num_components; ++ci) {
    jpeg_component_info& comp = info.comp_info[ci];
    // The size, in pixels, of one sample of this component.
    const int sx = info.max_h_samp_factor / comp.h_samp_factor;
    const int sy = info.max_v_samp_factor / comp.v_samp_factor;
    const UINT16* quantval = comp.quant_table->quantval;

    for (JDIMENSION by = 0; by < comp.height_in_blocks; ++by) {
      // The cells under this row of blocks: a block spans 8 samples, sy cells tall.
      const int cellTop = int(by) * sy;
      if (cellTop >= cellRows) {
        break;
      }
      const int cellBottom = std::min(cellRows, cellTop + sy);
      JBLOCKARRAY rowBuffer = nullptr;

      for (JDIMENSION bx = 0; bx < comp.width_in_blocks; ++bx) {
        const int cellLeft = int(bx) * sx;
        if (cellLeft >= cellsPerRow) {
          break;
        }
        const int cellRight = std::min(cellsPerRow, cellLeft + sx);
        bool changed = false;
        for (int cy = cellTop; (cy < cellBottom) && !changed; ++cy) {
          for (int cx = cellLeft; cx < cellRight; ++cx) {
            if (changedCells[size_t(cy) * cellsPerRow + cx]) {
              changed = true;
              break;
            }
          }
        }
        if (!changed) {
          continue;
        }

        if (!rowBuffer) {
          rowBuffer
              = (*info.mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(&info), coefArrays[ci], by, 1, TRUE);
        }

        // The samples, each the average of the sx * sy pixels it stands for.
        // Pixels past the edge of the image repeat the last row or column,
        // which is how libjpeg pads a partial block.
        double samples[8][8];
        for (int j = 0; j < 8; ++j) {
          for (int i = 0; i < 8; ++i) {
            const int px0 = (int(bx) * 8 + i) * sx;
            const int py0 = (int(by) * 8 + j) * sy;
            double sum = 0.0;
            for (int dy = 0; dy < sy; ++dy) {
              const int py = std::min(height - 1, py0 + dy);
              for (int dx = 0; dx < sx; ++dx) {
                const int px = std::min(width - 1, px0 + dx);
                sum += componentValue(edited.pixel(px, py), info.jpeg_color_space, ci);
              }
            }
            samples[j][i] = sum / (sx * sy) - 128.0;
          }
        }
        forwardDctQuantize(samples, quantval, rowBuffer[0][bx]);
      }
    }
  }
}

void copyMarkers(const jpeg_decompress_struct& src, jpeg_compress_struct& dst) {
  for (jpeg_saved_marker_ptr marker = src.marker_list; marker; marker = marker->next) {
    // libjpeg writes these two itself when asked to; a second copy would be invalid.
    if (dst.write_JFIF_header && (marker->marker == JPEG_APP0) && (marker->data_length >= 5)
        && (std::memcmp(marker->data, "JFIF\0", 5) == 0)) {
      continue;
    }
    if (dst.write_Adobe_marker && (marker->marker == JPEG_APP0 + 14) && (marker->data_length >= 5)
        && (std::memcmp(marker->data, "Adobe", 5) == 0)) {
      continue;
    }
    jpeg_write_marker(&dst, marker->marker, marker->data, marker->data_length);
  }
}
}  // namespace

QString jpegUnsupportedReason(const QByteArray& data) {
  const auto session = std::make_unique<Session>();
  if (setjmp(session->err.jump)) {
    return Tr::tr("it could not be read as JPEG (%1)").arg(QString::fromLocal8Bit(session->err.message));
  }
  startReading(*session, data);
  jpeg_read_header(&session->src, TRUE);
  return headerUnsupportedReason(session->src);
}

bool patchJpeg(const QByteArray& source,
               const QImage& original,
               const QImage& edited,
               const QRect& changedArea,
               QByteArray* result,
               QString* error) {
  const auto fail = [error](const QString& reason) {
    if (error) {
      *error = reason;
    }
    return false;
  };

  if (original.isNull() || (original.size() != edited.size()) || (original.format() != edited.format())) {
    return fail(Tr::tr("the edited image does not match the original"));
  }

  // Everything that allocates is done before the setjmp(): a longjmp must not
  // skip the destruction of anything created after it.
  const int cellsPerRow = (original.width() + 7) / 8;
  const int cellRows = (original.height() + 7) / 8;
  std::vector<unsigned char> changedCells(size_t(cellsPerRow) * cellRows, 0);
  const QRect area(changedArea.intersected(original.rect()));
  if (area.isEmpty() || !markChangedCells(original, edited, area, cellsPerRow, changedCells)) {
    *result = source;
    return true;
  }
  const auto session = std::make_unique<Session>();
  QString reason;

  if (setjmp(session->err.jump)) {
    return fail(Tr::tr("libjpeg: %1").arg(QString::fromLocal8Bit(session->err.message)));
  }

  startReading(*session, source);
  jpeg_save_markers(&session->src, JPEG_COM, 0xFFFF);
  for (int i = 0; i < 16; ++i) {
    jpeg_save_markers(&session->src, JPEG_APP0 + i, 0xFFFF);
  }
  jpeg_read_header(&session->src, TRUE);
  reason = headerUnsupportedReason(session->src);
  if (!reason.isEmpty()) {
    return fail(reason);
  }
  if ((int(session->src.image_width) != original.width()) || (int(session->src.image_height) != original.height())) {
    return fail(Tr::tr("the decoded image is %1x%2, but the file says %3x%4")
                    .arg(original.width())
                    .arg(original.height())
                    .arg(session->src.image_width)
                    .arg(session->src.image_height));
  }

  jvirt_barray_ptr* coefArrays = jpeg_read_coefficients(&session->src);
  patchBlocks(session->src, coefArrays, edited, changedCells, cellsPerRow, cellRows);

  jpeg_create_compress(&session->dst);
  session->dstCreated = true;
  jpeg_copy_critical_parameters(&session->src, &session->dst);
  session->dst.optimize_coding = TRUE;
  session->dst.arith_code = session->src.arith_code;
  session->dst.restart_interval = session->src.restart_interval;
  if (session->src.progressive_mode) {
    jpeg_simple_progression(&session->dst);
  }
  // Only the headers the file had: jpeg_copy_critical_parameters() would add a
  // JFIF header to, say, an EXIF-only file from a camera.
  session->dst.write_JFIF_header = session->src.saw_JFIF_marker;
  session->dst.write_Adobe_marker = session->src.saw_Adobe_marker;
  jpeg_mem_dest(&session->dst, &session->outBuffer, &session->outSize);
  jpeg_write_coefficients(&session->dst, coefArrays);
  copyMarkers(session->src, session->dst);
  jpeg_finish_compress(&session->dst);
  jpeg_finish_decompress(&session->src);

  *result = QByteArray(reinterpret_cast<const char*>(session->outBuffer), static_cast<int>(session->outSize));
  return true;
}
}  // namespace retouch
