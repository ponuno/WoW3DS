// inflate.hpp — small zlib (RFC 1950) / deflate (RFC 1951) decoder, no dependencies.
// Used for SMSG_COMPRESSED_UPDATE_OBJECT and SMSG_COMPRESSED_MOVES.
#pragma once
#include <cstddef>
#include <cstdint>

#include "crypto.hpp"

namespace w3n {

// Raw deflate stream -> out (appended). maxOut guards against bombs.
bool inflateRaw(const uint8_t* src, size_t n, Bytes& out, size_t maxOut, size_t* consumed = nullptr);
// zlib stream (2-byte header, deflate data, Adler-32). 'expected' is the size the
// server announced (0 = unknown); the result must match it exactly when given.
bool zlibInflate(const uint8_t* src, size_t n, Bytes& out, size_t expected, size_t maxOut = 8u << 20);
uint32_t adler32(const uint8_t* p, size_t n);

}  // namespace w3n
