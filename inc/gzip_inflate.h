#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inflate a gzip- or raw-DEFLATE stream into dst.
 * Returns 0 on success (out_len set), negative on error. */
int gzip_inflate(const uint8_t *src, size_t src_len,
                 uint8_t *dst, size_t dst_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
