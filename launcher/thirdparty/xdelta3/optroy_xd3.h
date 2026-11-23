/* Small wrapper around xdelta3's in-memory decoder for the launcher (so C++ code
 * doesn't depend on xdelta3's size type settings). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  OPTROY_XD3_OK = 0,
  OPTROY_XD3_NO_SPACE = 1,  /* output bigger than avail_output: retry with more */
  OPTROY_XD3_BAD = 2,       /* not a valid patch for this file (checksum, corrupt) */
  OPTROY_XD3_TOO_BIG = 3,   /* sizes beyond what the decoder handles */
};

/* Applies the xdelta / VCDIFF patch `patch` to `source`. On success *out_size is
 * the patched size. `message` (may be NULL) gets a short reason on failure. */
int optroy_xd3_decode(const uint8_t* patch, size_t patch_size, const uint8_t* source, size_t source_size,
                      uint8_t* out, size_t avail_output, size_t* out_size, const char** message);

#ifdef __cplusplus
}
#endif
