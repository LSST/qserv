// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2019 AURA/LSST.
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

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "loader/CompositeKey.h"
#include "loader/ConfigBase.h"

// Boost unit test header
#define BOOST_TEST_MODULE LoaderConfig
#include "boost/test/included/unit_test.hpp"


namespace test = boost::test_tools;
using namespace lsst::qserv::loader;

BOOST_AUTO_TEST_SUITE(Suite)

BOOST_AUTO_TEST_CASE(LoaderTest) {

    LOG_INFO("LoaderConfig test start");
    ConfigElement::CfgElementList cfgElemList;
    std::string header("hdr");

    LOG_INFO("Test valid values");

    {
        auto vString = ConfigElement::create(cfgElemList, header, "str1", ConfigElement::STRING, true);
        vString->setValue("a string");
        BOOST_CHECK(vString->verifyValueIsOfKind());
    }

    {
        auto vInt = ConfigElement::create(cfgElemList, header, "vInt1", ConfigElement::INT, true);
        vInt->setValue("1234567890");
        BOOST_CHECK(vInt->verifyValueIsOfKind());
    }

    {
        auto vInt = ConfigElement::create(cfgElemList, header, "vInt2", ConfigElement::INT, true);
        vInt->setValue("0");
        BOOST_CHECK(vInt->verifyValueIsOfKind());
    }

    {
        auto vInt = ConfigElement::create(cfgElemList, header, "vInt3", ConfigElement::INT, true);
        vInt->setValue("-1");
        BOOST_CHECK(vInt->verifyValueIsOfKind());
    }

    {
        auto vInt = ConfigElement::create(cfgElemList, header, "vInt4", ConfigElement::INT, true);
        vInt->setValue("+7");
        BOOST_CHECK(vInt->verifyValueIsOfKind());
    }

    {
        auto vFloat = ConfigElement::create(cfgElemList, header, "vFloat1", ConfigElement::FLOAT, true);
        vFloat->setValue("1234567890.0987654321");
        BOOST_CHECK(vFloat->verifyValueIsOfKind());
    }

    {
        auto vFloat = ConfigElement::create(cfgElemList, header, "vFloat2", ConfigElement::FLOAT, true);
        vFloat->setValue("0");
        BOOST_CHECK(vFloat->verifyValueIsOfKind());
    }

    {
        auto vFloat = ConfigElement::create(cfgElemList, header, "vFloat3", ConfigElement::FLOAT, true);
        vFloat->setValue(".01");
        BOOST_CHECK(vFloat->verifyValueIsOfKind());
    }

    {
        auto vFloat = ConfigElement::create(cfgElemList, header, "vFloat4", ConfigElement::FLOAT, true);
        vFloat->setValue("-.01");
        BOOST_CHECK(vFloat->verifyValueIsOfKind());
    }

    {
        auto vFloat = ConfigElement::create(cfgElemList, header, "vFloat5", ConfigElement::FLOAT, true);
        vFloat->setValue("-.01");
        BOOST_CHECK(vFloat->verifyValueIsOfKind());
    }

    {
        auto vFloat = ConfigElement::create(cfgElemList, header, "vFloat5", ConfigElement::FLOAT, true);
        vFloat->setValue("+0.01");
        BOOST_CHECK(vFloat->verifyValueIsOfKind());
    }

    {
        auto vFloat = ConfigElement::create(cfgElemList, header, "vFloat6", ConfigElement::FLOAT, true);
        vFloat->setValue("1.03e-2");
        BOOST_CHECK(vFloat->verifyValueIsOfKind());
    }

    LOG_INFO("Test bad values");
    /// There aren't any rules about what would be an invalid STRING

    {
        auto bInt = ConfigElement::create(cfgElemList, header, "bInt1", ConfigElement::INT, true);
        bInt->setValue(" 1234567890a ");
        BOOST_CHECK(not bInt->verifyValueIsOfKind());
    }

    {
        auto bInt = ConfigElement::create(cfgElemList, header, "bInt2", ConfigElement::INT, true);
        bInt->setValue(" ");
        BOOST_CHECK(not bInt->verifyValueIsOfKind());
    }

    {
        auto bInt = ConfigElement::create(cfgElemList, header, "bInt3", ConfigElement::INT, true);
        bInt->setValue("z");
        BOOST_CHECK(not bInt->verifyValueIsOfKind());
    }

    {
        auto bInt = ConfigElement::create(cfgElemList, header, "bInt3", ConfigElement::INT, true);
        bInt->setValue("-");
        BOOST_CHECK(not bInt->verifyValueIsOfKind());
    }

    {
        auto bInt = ConfigElement::create(cfgElemList, header, "bInt3", ConfigElement::INT, true);
        bInt->setValue("+");
        BOOST_CHECK(not bInt->verifyValueIsOfKind());
    }

    {
        auto bInt = ConfigElement::create(cfgElemList, header, "bInt3", ConfigElement::INT, true);
        bInt->setValue("1.7");
        BOOST_CHECK(not bInt->verifyValueIsOfKind());
    }

    {
        auto bFloat = ConfigElement::create(cfgElemList, header, "bFloat1", ConfigElement::FLOAT, true);
        bFloat->setValue(" 1234567890a ");
        BOOST_CHECK(not bFloat->verifyValueIsOfKind());
    }

    {
        auto bFloat = ConfigElement::create(cfgElemList, header, "bFloat2", ConfigElement::FLOAT, true);
        bFloat->setValue(" ");
        BOOST_CHECK(not bFloat->verifyValueIsOfKind());
    }

    {
        auto bFloat = ConfigElement::create(cfgElemList, header, "bFloat3", ConfigElement::FLOAT, true);
        bFloat->setValue("z");
        BOOST_CHECK(not bFloat->verifyValueIsOfKind());
    }

    {
        auto bFloat = ConfigElement::create(cfgElemList, header, "bFloat4", ConfigElement::FLOAT, true);
        bFloat->setValue("-");
        BOOST_CHECK(not bFloat->verifyValueIsOfKind());
    }

    {
        auto bFloat = ConfigElement::create(cfgElemList, header, "bFloat5", ConfigElement::FLOAT, true);
        bFloat->setValue("+");
        BOOST_CHECK(not bFloat->verifyValueIsOfKind());
    }

    {
        auto bFloat = ConfigElement::create(cfgElemList, header, "bFloat5", ConfigElement::FLOAT, true);
        bFloat->setValue(".");
        BOOST_CHECK(not bFloat->verifyValueIsOfKind());
    }

    LOGS_INFO("LoaderConfig test end");


    LOG_INFO("CompositeKey test start");

    {
        LOGS_INFO("Comparisons to self");
        CompositeKey a();
        BOOST_CHECK(a == a);
        BOOST_CHECK(!(a != a));
        BOOST_CHECK(!(a < a));
        BOOST_CHECK(!(a > a));
        BOOST_CHECK(a <= a);
        BOOST_CHECK(a >= a);
    }

    {
        LOGS_INFO("Comparisons integer equal");
        CompositeKey a(9876);
        CompositeKey b(9876);
        BOOST_CHECK(a == b);
        BOOST_CHECK(!(a != b));
        BOOST_CHECK(!(a < b));
        BOOST_CHECK(!(a > b));
        BOOST_CHECK(a <= b);
        BOOST_CHECK(a >= b);
    }

    {
        LOGS_INFO("Comparisons integer less than");
        CompositeKey a(875);
        CompositeKey b(876);
        BOOST_CHECK(!(a == b));
        BOOST_CHECK( (a != b));
        BOOST_CHECK( (a < b));
        BOOST_CHECK(!(a > b));
        BOOST_CHECK( (a <= b));
        BOOST_CHECK(!(a >= b));
    }

    {
        LOGS_INFO("Comparisons integer greater than");
        CompositeKey a(1000000);
        CompositeKey b(30);
        BOOST_CHECK(!(a == b));
        BOOST_CHECK( (a != b));
        BOOST_CHECK(!(a < b));
        BOOST_CHECK( (a > b));
        BOOST_CHECK(!(a <= b));
        BOOST_CHECK( (a >= b));
    }

    {
        LOGS_INFO("Comparisons integer greater than");
        CompositeKey a(1000000, "a");
        CompositeKey b(30, "b");
        BOOST_CHECK(!(a == b));
        BOOST_CHECK( (a != b));
        BOOST_CHECK(!(a < b));
        BOOST_CHECK( (a > b));
        BOOST_CHECK(!(a <= b));
        BOOST_CHECK( (a >= b));
    }

    {
        LOGS_INFO("Comparisons string equal");
        CompositeKey a(0, "string%$testA");
        CompositeKey b(0, "string%$testA");
        BOOST_CHECK(a == b);
        BOOST_CHECK(!(a != b));
        BOOST_CHECK(!(a < b));
        BOOST_CHECK(!(a > b));
        BOOST_CHECK(a <= b);
        BOOST_CHECK(a >= b);
    }

    {
        LOGS_INFO("Comparisons string less than");
        CompositeKey a(875, "testa");
        CompositeKey b(875, "testb");
        BOOST_CHECK(!(a == b));
        BOOST_CHECK( (a != b));
        BOOST_CHECK( (a < b));
        BOOST_CHECK(!(a > b));
        BOOST_CHECK( (a <= b));
        BOOST_CHECK(!(a >= b));
    }

    {
        LOGS_INFO("Comparisons string greater than");
        CompositeKey a(30, "testd");
        CompositeKey b(30, "testc");
        BOOST_CHECK(!(a == b));
        BOOST_CHECK( (a != b));
        BOOST_CHECK(!(a < b));
        BOOST_CHECK( (a > b));
        BOOST_CHECK(!(a <= b));
        BOOST_CHECK( (a >= b));
    }

    {
        CompositeKey a(34568, "@#WSR$RT%fewsewer");
        CompositeKey b(a);
        BOOST_CHECK(b == a);
    }

    {
        CompositeKey b;
        CompositeKey a(98763, "AsdE$%342");
        b = a;
        BOOST_CHECK(a == b);
    }

    LOGS_INFO("CompositeKey test end");
}

BOOST_AUTO_TEST_SUITE_END()
