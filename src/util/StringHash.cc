// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2015 AURA/LSST.
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */
// StringHash -- Little wrappers for OpenSSL hashing.

// Class header
#include "util/StringHash.h"

// System headers
#include <iostream>
#include <sstream>

// Third-party headers
#ifdef __APPLE__
#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#endif

namespace {

#ifdef __APPLE__

// Wrappers for CommonCrytpo on OSX to adapt function names and signatures to look like
// their corresponding openssl versions.

unsigned char* WMD5(unsigned char const* data, size_t len, unsigned char* md) {
    return CC_MD5(static_cast<void const*>(data), static_cast<CC_LONG>(len), md);
}

unsigned char* SHA1(unsigned char const* data, size_t len, unsigned char* md) {
    return CC_SHA1(static_cast<void const*>(data), static_cast<CC_LONG>(len), md);
}

unsigned char* SHA256(unsigned char const* data, size_t len, unsigned char* md) {
    return CC_SHA256(static_cast<void const*>(data), static_cast<CC_LONG>(len), md);
}

#else  // !__APPLE__

unsigned char* WMD5(unsigned char const* data, size_t len, unsigned char* md) {
    EVP_Q_digest(NULL, "MD5", NULL, data, len, md, NULL);
    return NULL;
}

#endif  // !__APPLE__

template <unsigned char* dFunc(unsigned char const*, size_t, unsigned char*), int dLength>
inline std::string wrapHash(void const* buffer, int bufferSize) {
    unsigned char digest[dLength];
    dFunc(reinterpret_cast<unsigned char const*>(buffer), bufferSize, digest);
    return std::string(reinterpret_cast<char*>(digest), dLength);
}

template <unsigned char* dFunc(unsigned char const*, size_t, unsigned char*), int dLength>
inline std::string wrapHashHex(void const* buffer, int bufferSize) {
    unsigned char digest[dLength];
    dFunc(reinterpret_cast<unsigned char const*>(buffer), bufferSize, digest);
    // C++ stream version is ~30x faster than boost::format version.
    // i.e. (boost::format("%02x") % static_cast<int>(hashVal[i])).str();
    std::ostringstream s;
    s.flags(std::ios::hex);
    s.fill('0');
    for (int i = 0; i < dLength; ++i) {
        s.width(2);
        s << static_cast<unsigned int>(digest[i]);
    }
    return s.str();
}

}  // anonymous namespace

namespace lsst::qserv::util {

/// @return a hexadecimal representation of the MD5 hash of the input buffer
/// 128 bits -> 16 bytes -> 32 hex digits
std::string StringHash::getMd5Hex(char const* buffer, int bufferSize) {
    return wrapHashHex<WMD5, MD5_DIGEST_LENGTH>(buffer, bufferSize);
}

/// @return a hexadecimal representation of the SHA1 hash of the input buffer
/// 160 bits -> 20 bytes -> 40 hex digits
std::string StringHash::getSha1Hex(char const* buffer, int bufferSize) {
    return wrapHashHex<SHA1, SHA_DIGEST_LENGTH>(buffer, bufferSize);
}
/// @return a hexadecimal representation of the SHA256 hash of the input buffer
/// 256 bits -> 32 bytes -> 64 hex digits
std::string StringHash::getSha256Hex(char const* buffer, int bufferSize) {
    return wrapHashHex<SHA256, SHA256_DIGEST_LENGTH>(buffer, bufferSize);
}

/// @return the raw SHA1 hash of the input buffer
/// 160 bits -> 20 bytes
std::string StringHash::getSha1(char const* buffer, int bufferSize) {
    return wrapHash<SHA1, SHA_DIGEST_LENGTH>(buffer, bufferSize);
}

/// @return the raw SHA256 hash of the input buffer
/// 256 bits -> 32 bytes
std::string StringHash::getSha256(char const* buffer, int bufferSize) {
    return wrapHash<SHA256, SHA256_DIGEST_LENGTH>(buffer, bufferSize);
}

}  // namespace lsst::qserv::util
