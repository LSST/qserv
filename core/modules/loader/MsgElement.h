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
#ifndef LSST_QSERV_LOADER_MSGELEMENT_H
#define LSST_QSERV_LOADER_MSGELEMENT_H

// system headers
#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <string>

// Qserv headers
#include "loader/BufferUdp.h"
#include "proto/ProtoImporter.h"
#include "util/Issue.h"


#define MAX_MSG_STRING_LENGTH 5000

namespace lsst {
namespace qserv {
namespace loader {

/// Class for throwing communication/parsing exceptions.
class LoaderMsgErr : public util::Issue {
public:
    LoaderMsgErr(util::Issue::Context const& ctx, std::string const& message) :
        util::Issue(ctx, message) {}
};

/// Base class for message elements. It include methods for appending or retrieving
/// the different types of MsgElements from BufferUdp objects.
/// Parsing and communication errors may throw LoaderMsgErr.
class MsgElement {
public:
    using Ptr = std::shared_ptr<MsgElement>;
    enum ElementType {
        NOTHING     = 0,
        STRING_ELEM = 1,
        UINT16_ELEM = 2,
        UINT32_ELEM = 3,
        UINT64_ELEM = 4
    };

    explicit MsgElement(char elementType) : _elementType(elementType) {}
    MsgElement() = delete;
    MsgElement(MsgElement const&) = delete;
    MsgElement& operator=(MsgElement const&) = delete;
    virtual  ~MsgElement() = default;

    /// This method appends the data in the MsgElement to 'data'.
    /// Pointers in 'data are updated appropriately.
    virtual bool appendToData(BufferUdp& data)=0;

    /// This method retrieves a single MsgElement from 'data'. Pointers
    /// in 'data' are moved to where the next MsgElement should be located.
    /// @return true if a MsgElement could be read safely, otherwise
    ///         it returns false.
    virtual bool retrieveFromData(BufferUdp &data)=0;

    /// Return the TRANSMITTED size of the element. For StringElement, this is not know until
    /// the string has been constructed. Numeric elements have constant size.
    virtual size_t transmitSize() const =0;

    /// Create the correct MsgElement child class for 'elemenetType'
    static MsgElement::Ptr create(char elementType);

    /// Retrieve the type of the element in 'data' and put the type in 'elemType'.
    /// The value in 'elemType' is only valid if the method returns true. Pointers
    /// in 'data' are moved appropriately.
    /// @return - True if a type could be retrieved from 'data'. False otherwise.
    static bool retrieveType(BufferUdp &data, char& elemType);

    /// Retrieve a MsgElement from 'data' and return it. Pointers in 'data'
    /// are updated appropriately.
    /// @return a pointer to the MsgElement retrieved or nullptr if
    ///         no MsgElement could be retrieved.
    static MsgElement::Ptr retrieve(BufferUdp& data);

    /// @return True if 'a' and 'b' are equivalent. False otherwise.
    static bool equal(MsgElement* a, MsgElement* b) {
        if (a == b) return true;
        if (a == nullptr || b == nullptr) return false;
        if (a->_elementType != b->_elementType) return false;
        if (a->_elementType == NOTHING) return true;
        return a->equal(b);
    }

    /// @return the type of this MsgElement.
    char getElementType() const { return _elementType; }

    /// @return a string that is a good representation of what is in the MsgElement.
    static std::string getStringVal(MsgElement::Ptr const& msgElem) {
        if (msgElem == nullptr) return std::string("nullptr");
        return msgElem->getStringVal();
    }

    /// @return a string that is a good representation of what is in the MsgElement.
    virtual std::string getStringVal()=0;

    /// @return true if this MsgElement is equivalent to 'other'
    virtual bool equal(MsgElement* other)=0;

    /// @return the size of the base class data members in bytes.
    size_t sizeOfBase() const { return sizeof(_elementType); }

protected:
    /// Append _elementType to 'data' advancing pointers appropriately.
    /// @return true if _elementType was successfully appended.
    bool _appendType(BufferUdp &data) const {
        return data.append(&_elementType, sizeof(_elementType));
    }

    /// Get the type from from 'data'.
    /// @return a pointer to the byte immediately after the type information in 'data'.
    const char* _retrieveType(const char* data) {
        _elementType = *data;
        const char* ptr = data + 1;
        return ptr;
    }

private:
    char _elementType{NOTHING}; ///< The type of this MsgElement.
};


/// Generic numeric type for network transmission. The class provides
/// big<->little endian conversion for network transfers as well as
/// definitions for the virtual functions in MsgElement.
template <class T, typename = typename std::enable_if<std::is_arithmetic<T>::value, T>::type>
class NumElement : public MsgElement {
public:
    explicit NumElement(char myType) : MsgElement(myType) {}
    NumElement(char myType, T element_) : MsgElement(myType), element(element_) {}
    NumElement() = delete;
    NumElement(NumElement<T> const&) = delete;
    NumElement& operator=(NumElement<T> const&) = delete;

    bool appendToData(BufferUdp &data) override {
        if (_appendType(data)) {
            T netElem = changeEndianessOnLittleEndianOnly(element);
            return data.append(&netElem, _sizeT);
        }
        return false;
    }

    bool retrieveFromData(BufferUdp &data) override {
        T netElem;
        if (data.retrieve(&netElem, sizeof(T))) {
            element = changeEndianessOnLittleEndianOnly(netElem);
            return true;
        }
        return false;
    }

    size_t transmitSize() const override {
        return _sizeT + sizeOfBase();
    }

    std::string getStringVal() override { return std::to_string(element); }

    T element{0}; ///< The actual numeric value of this MsgElement.

    /// This function will change endianess only on little endian machines.
    /// It is effectively a no-op on big endian machines.
    T changeEndianessOnLittleEndianOnly(T const& in) {
        uint8_t data[_sizeT];
        memcpy(&data, &in, _sizeT);
        T res = 0;
        int shift = 0;
        int pos = _sizeT -1;
        for (size_t j=0; j < _sizeT; ++j) {
            res |= static_cast<T>(data[pos]) << shift;
            shift += 8;
            --pos;
        }
        return res;
    }

    bool equal(MsgElement* other) override {
        NumElement<T>* ptr = dynamic_cast<NumElement<T>*>(other);
        if (ptr == nullptr) return false;
        return (element == ptr->element);
    }

private:
    static const size_t _sizeT{sizeof(T)}; ///< Size of the numeric type in bytes.
};


class UInt16Element : public NumElement<uint16_t> {
public:
    using Ptr = std::shared_ptr<UInt16Element>;
    static const int MYTYPE = UINT16_ELEM;

    UInt16Element() : NumElement(MYTYPE) {}
    explicit UInt16Element(uint16_t element_) : NumElement(MYTYPE, element_) {}
    UInt16Element(UInt16Element const&) = delete;
    UInt16Element& operator=(UInt16Element const&) = delete;
};


class UInt32Element : public NumElement<uint32_t> {
public:
    using Ptr = std::shared_ptr<UInt32Element>;
    static const int MYTYPE = UINT32_ELEM;

    UInt32Element() : NumElement(MYTYPE) {}
    explicit UInt32Element(uint32_t element_) : NumElement(MYTYPE, element_) {}
    UInt32Element(UInt32Element const&) = delete;
    UInt32Element& operator=(UInt32Element const&) = delete;
};


class UInt64Element : public NumElement<uint64_t> {
public:
    using Ptr = std::shared_ptr<UInt64Element>;
    static const int MYTYPE = UINT64_ELEM;

    UInt64Element() : NumElement(MYTYPE) {}
    explicit UInt64Element(uint64_t element_) : NumElement(MYTYPE, element_) {}
    UInt64Element(UInt64Element const&) = delete;
    UInt64Element& operator=(UInt64Element const&) = delete;
};



// Using uint32_t (LEN_TYPE) for length in all cases.
class StringElement : public MsgElement {
public:
    using Ptr = std::shared_ptr<StringElement>;
    using UPtr = std::unique_ptr<StringElement>;
    static const int MYTYPE = STRING_ELEM;
    typedef uint32_t S_LEN_TYPE; // If this type changes, so must associated htonl and ntohl calls.

    StringElement(std::string const& element_) : MsgElement(MYTYPE), element(element_) {}
    StringElement() : MsgElement(MYTYPE) {}
    StringElement(StringElement const&) = delete;
    StringElement& operator=(StringElement const&) = delete;

    ~StringElement() override = default;

    bool appendToData(BufferUdp& data) override;
    bool retrieveFromData(BufferUdp& data) override;
    std::string getStringVal() override { return element; }

    std::string element;

    bool equal(MsgElement* other) override {
        StringElement* ptr = dynamic_cast<StringElement*>(other);
        if (ptr == nullptr) { return false; }
        return (ptr->element == ptr->element);
    }

    /// @return true if this element is equal to other.
    /// 'os' is filled with a description of the comparison.
    bool compare(StringElement* other, std::ostream& os);

    /// The size of StringElement changes!
    size_t transmitSize() const override {
        // char in string, variable to transmit string length, size of base class.
        return element.size()+ sizeof(S_LEN_TYPE) + sizeOfBase();
    }

    template<typename T>
    std::unique_ptr<T> protoParse() {
        std::unique_ptr<T> protoItem(new T());
        bool success = proto::ProtoImporter<T>::setMsgFrom(*protoItem, element.data(), element.length());
        if (not success) {
            return nullptr;
        }
        return protoItem;
    }

    /// This function is only usable if it is know that all data has been read from the socket.
    /// This the case with UDP, and boost asio async reads that return after X bytes read.
    template<typename T>
    static std::unique_ptr<T> protoParse(BufferUdp& data) {
        StringElement::Ptr itemData = std::dynamic_pointer_cast<StringElement>(MsgElement::retrieve(data));
        if (itemData == nullptr) { return nullptr; }
        return itemData->protoParse<T>();
    }
};

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_MSGELEMENT_H
