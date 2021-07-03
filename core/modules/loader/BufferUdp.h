// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
 *
 */
#ifndef LSST_QSERV_LOADER_BUFFERUDP_H
#define LSST_QSERV_LOADER_BUFFERUDP_H

// system headers
#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <sstream>
#include <string>

// third party headers
#include <boost/asio.hpp>
#include <boost/bind.hpp>


namespace lsst {
namespace qserv {
namespace loader {

class MsgElement;


/// A buffer for reading and writing. Nothing can be read from the buffer until
/// something has written to it.
class BufferUdp {
public:
    using Ptr = std::shared_ptr<BufferUdp>;

    /// The absolute largest UDP message we would send.
    /// Usually, they should be much smaller.
    static int const MAX_MSG_SIZE = 6000;

    /// Create the object with a new _buffer with 'length' bytes.
    explicit BufferUdp(size_t length = MAX_MSG_SIZE) : _length(length) {
        _buffer = new char[length];
        _ourBuffer = true;
        _setupBuffer();
    }


    /// Create a BufferUdp object that uses 'buf' as its buffer, with 'length'
    /// indicating the number of bytes in the buffer. If 'buf' already
    /// contains valid data, 'validBytes' must be set to how many bytes of the buffer
    /// are valid.
    /// If BufferUdp should take ownership of 'buf', i.e. delete 'buf' when it is done,
    /// call makeOwnerOfBuffer().
    BufferUdp(char* buf, size_t length, size_t validBytes) : _buffer(buf), _length(length) {
        _setupBuffer();
        advanceWriteCursor(validBytes);
    }

    BufferUdp(BufferUdp const&) = delete;
    BufferUdp& operator=(BufferUdp const&) = delete;

    ~BufferUdp() { if (_ourBuffer) { delete[] _buffer; } }

    /// Resets the cursors in the buffer so it is effectively empty.
    void reset() { _setupBuffer(); }

    /// Return true only if this object owns the buffer.
    bool releaseOwnership();

    /// Make this object is responsible for deleting _buffer.
    void makeOwnerOfBuffer() { _ourBuffer = true; }

    /// Return true if there's at least 'len' room left in the buffer.
    bool isAppendSafe(size_t len) const { return (_wCursor + len) <= _end; }

    /// Append 'len' bytes at 'in' to the end of _buffer, but only if it is safe to do so.
    bool append(const void* in, size_t len);

    /// Advance the write cursor. This is usually needed after some other object has been
    /// allowed to write directly to the buffer. (boost::asio)
    void advanceWriteCursor(size_t len);

    /// Advance the read cursor, which usually needs to be done after another object
    /// has been allowed to read directly from the buffer. (boost::asio)
    void advanceReadCursor(size_t len);

    /// Repeatedly read a socket until a valid MsgElement is read, eof, or an error occurs.
    /// Errors throw LoaderMsgErr
    std::shared_ptr<MsgElement> readFromSocket(boost::asio::ip::tcp::socket& socket, std::string const& note);

    /// Return the total length of _buffer.
    size_t getMaxLength() const { return _length; }

    /// Returns the number of bytes left to be read from the buffer.
    int getBytesLeftToRead() const { return _wCursor - _rCursor; }

    /// Returns the amount of room left in the buffer after the write cursor.
    size_t getAvailableWriteLength() const { return _end - _wCursor; }

    /// Returns a char* pointing to data to be read from the buffer.
    const char* getReadCursor() const { return _rCursor; }

    /// Returns a char* pointing to where data should be written to the buffer.
    char* getWriteCursor() const { return _wCursor; }

    /// Returns true if retrieving 'len' bytes from the buffer will not violate the buffer rules.
    bool isRetrieveSafe(size_t len) const;

    /// Returns true if 'len' bytes could be copied to out without violating _buffer rules.
    bool retrieve(void* out, size_t len);

    /// Returns true if 'len' bytes could be copied to 'out' without violating _buffer rules.
    bool retrieveString(std::string& out, size_t len);

    /// Dumps basic data to a string. If 'hexDump' is true, include a complete dump of
    /// _buffer in hex.
    std::string dumpStr(bool hexDump=true) const { return dumpStr(hexDump, false); }

    /// Dumps basic data to a string. If 'hexDump' is true, include a complete dump of
    /// _buffer in hex. If 'charDump' is true, include a complete dump of the buffer
    /// in ascii.
    std::string dumpStr(bool hexDump, bool charDump) const;

    std::ostream& dump(std::ostream &os, bool hexDump, bool charDump) const;

private:
    void _setupBuffer() {
        _end = _buffer + _length;
        _wCursor = _buffer;
        _rCursor = _buffer;
    }

    /// Parse the valid portion of _buffer (starting at _rCursor) and see if one
    /// MsgElement is available. If so, return the element and advance _rCursor.
    /// Otherwise return nullptr.
    /// If a message is not recovered, the buffer is left effectively unchanged.
    std::shared_ptr<MsgElement> _safeRetrieve();


    char* _buffer;
    size_t _length;  ///< Number of bytes in the array (total capacity of array).
    char* _end;      ///< Immediately after the last element in the array.
    char* _wCursor;  ///< Where new elements will be appended to the array.
    const char* _rCursor; ///< Where data is read from the buffer.

    bool _ourBuffer{false}; ///< true if this class object is responsible for deleting the buffer.
};

/// Print basic buffer information. Use BufferUdp::dump() directly if the buffer contents are needed.
std::ostream& operator<<(std::ostream& os, BufferUdp const& buf);

}}} // namespace lsst:qserv:loader

#endif // LSST_QSERV_LOADER_BUFFERUDP_H
