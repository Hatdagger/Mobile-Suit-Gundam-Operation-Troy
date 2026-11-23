/* See optroy_xd3.h. Built with xdelta3.c (Apache License 2.0, see LICENSE). */
#include "optroy_xd3.h"

#include "xdelta3.h"

#include <errno.h>

int optroy_xd3_decode(const uint8_t* patch, size_t patch_size, const uint8_t* source, size_t source_size,
                      uint8_t* out, size_t avail_output, size_t* out_size, const char** message) {
  usize_t produced = 0;
  int ret;
  if (message) *message = "";
  if ((size_t)(usize_t)patch_size != patch_size || (size_t)(usize_t)source_size != source_size ||
      (size_t)(usize_t)avail_output != avail_output) {
    if (message) *message = "file too big";
    return OPTROY_XD3_TOO_BIG;
  }
  ret = xd3_decode_memory(patch, (usize_t)patch_size, source, (usize_t)source_size, out, &produced,
                          (usize_t)avail_output, 0);
  if (ret == 0) {
    *out_size = (size_t)produced;
    return OPTROY_XD3_OK;
  }
  if (ret == ENOSPC) {
    if (message) *message = "output buffer too small";
    return OPTROY_XD3_NO_SPACE;
  }
  if (message) *message = ret == XD3_INVALID_INPUT ? "patch does not match this file" : xd3_strerror(ret);
  return OPTROY_XD3_BAD;
}
