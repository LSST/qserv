// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 AURA/LSST.
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

// Class header
#include "BufferUdp.h"

// qserv headers
#include "loader/LoaderMsg.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.BufferUdp");
}


namespace lsst {
namespace qserv {
namespace loader {


MsgElement::Ptr BufferUdp::readFromSocket(boost::asio::ip::tcp::socket& socket, std::string const& note) {
    // Repeatedly read a socket until a valid MsgElement is read, eof,  or an error occurs.
    for (;;) {
        LOGS(_log, LOG_LVL_DEBUG, note << " readFromSocket");
        boost::system::error_code error;

        // If there's something in the buffer already, get it and return.
        // This can happen when the previous read of socket read multiple elements.
        MsgElement::Ptr msgElem = _safeRetrieve();
        if (msgElem != nullptr) {
            return msgElem;
        }

        size_t len = socket.read_some(boost::asio::buffer(_wCursor, getAvailableWriteLength()), error);
        _wCursor += len; /// must advance the cursor.

        // EOF is only a problem if no MsgElement was retrieved.
        // ??? This is definitely the case in UDP, EOF as nothing more will show up.
        // ??? But TCP is another issue as EOF is returned when the connection is still live but
        // ??? there's no data (len=0). Why does read_some set error to eof before the tcp connection is closed?
        if (error == boost::asio::error::eof) {
            LOGS(_log, LOG_LVL_WARN, "readFromSocket eof");
            break; // Connection closed cleanly by peer.
        } else if (error && error != boost::asio::error::eof) {
            throw LoaderMsgErr(ERR_LOC, "BufferUdp::readFromSocket note=" + note + " asio error=" + error.message());
        }

        /// Try to retrieve an element (there's no guarantee that an entire element got read in a single read.
        // Store original cursor positions so they can be restored if the read fails.
        msgElem = _safeRetrieve();
        if (msgElem != nullptr) {
            return msgElem;
        }
    }
    return nullptr;
}


bool BufferUdp::releaseOwnership() {
    if (_ourBuffer) {
        _ourBuffer = false;
        return true;
    }
    return false;
}


bool BufferUdp::append(const void* in, size_t len) {
    if (isAppendSafe(len)) {
        memcpy(_wCursor, in, len);
        _wCursor += len;
        return true;
    }
    return false;
}


void BufferUdp::advanceWriteCursor(size_t len) {
    _wCursor += len;
    if (not isAppendSafe(0)) {
        // The buffer has overflowed.
        throw std::overflow_error("BufferUdp advanceWriteCursor beyond buffer len=" +
                                  std::to_string(len));
    }
}


void BufferUdp::advanceReadCursor(size_t len) {
    _rCursor += len;
    if (_rCursor > _end) {
        // Something read data outside of the buffer range.
        throw std::overflow_error("BufferUdp advanceReadCursor beyond buffer len=" +
                                  std::to_string(len));
    }
}


std::shared_ptr<MsgElement> BufferUdp::_safeRetrieve() {
    auto wCursorOriginal = _wCursor;
    auto rCursorOriginal = _rCursor;
    MsgElement::Ptr msgElem = MsgElement::retrieve(*this);
    if (msgElem != nullptr) {
        return msgElem;
    } else {
        _wCursor = wCursorOriginal;
        _rCursor = rCursorOriginal;
    }
    return nullptr;
}


bool BufferUdp::isRetrieveSafe(size_t len) const {
    auto newLen = (_rCursor + len);
    return (newLen <= _end && newLen <= _wCursor);
}


bool BufferUdp::retrieve(void* out, size_t len) {
    if (isRetrieveSafe(len)) {
        memcpy(out, _rCursor, len);
        _rCursor += len;
        return true;
    }
    return false;
}


bool BufferUdp::retrieveString(std::string& out, size_t len) {
    if (isRetrieveSafe(len)) {
        const char* strEnd = _rCursor + len;
        std::string str(_rCursor, strEnd);
        _rCursor = strEnd;
        out = str;
        return true;
    }
    return false;
}


std::string BufferUdp::dumpStr(bool hexDump, bool charDump) const {
    std::stringstream os;
    dump(os, hexDump, charDump);
    return os.str();
}


std::ostream& BufferUdp::dump(std::ostream &os, bool hexDump, bool charDump) const {
    os << "maxLength=" << _length;
    os << " buffer=" << (void*)_buffer;
    os << " wCurLen=" << getAvailableWriteLength();
    os << " wCursor=" << (void*)_wCursor;
    os << " rCurLen=" << getBytesLeftToRead();
    os << " rCursor=" << (void*)_rCursor;
    os << " end=" << (void*)_end;

    // hex dump
    if (hexDump) {
        os << "(";
        for (const char* j=_buffer; j < _wCursor; ++j) {
            os << std::hex << (int)*j << " ";
        }
        os << ")";
    }

    // character dump
    if (charDump) {
        os << "(" << std::string(_buffer, _wCursor) << ")";
    }

    return os;
}


std::ostream& operator<<(std::ostream& os, BufferUdp const& buf) {
    return buf.dump(os, false, false);
}

}}} // namespace lsst:qserv:loader
