// -*- LSST-C++ -*-

/*
 * LSST Data Management System
 * Copyright 2015-2016 AURA/LSST.
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

/**
 * @file KvInterfaceImplMySql.h
 *
 * @brief Interface to the Central State System - MySql-based implementation.
 *
 * @Author Nathan Pease, SLAC
 */

// Class header
#include "css/KvInterfaceImplMySql.h"

// System headers
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

// Third-party headers
#define BOOST_BIND_GLOBAL_PLACEHOLDERS 1
#include "boost/algorithm/string.hpp"
#include "boost/algorithm/string/predicate.hpp"
#include "boost/algorithm/string/split.hpp"
#include "boost/format.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/property_tree/ptree.hpp"
#include "boost/property_tree/json_parser.hpp"
#include "mysql/mysqld_error.h"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "css/CssError.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"
#include "sql/SqlResults.h"
#include "sql/SqlTransactionScope.h"

using std::map;
using std::string;
using std::vector;
namespace ptree = boost::property_tree;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.css.KvInterfaceImplMySql");

const char KEY_PATH_DELIMITER('/');

using lsst::qserv::css::CssError;
using lsst::qserv::sql::SqlErrorObject;
using lsst::qserv::sql::SqlResults;

/**
 * @brief Helper for getting the value from an INT row
 * @param results the object containing results information
 * @param value the output var to return the result
 * @return true if an int result could be obtained else false
 */
bool extractIntValueFromSqlResults(SqlResults* results, unsigned int* value) {
    // ideally results would be a sql::SqlResults const*, but extractFirstValue is non-const
    if (NULL == results || NULL == value) {
        throw CssError(ERR_LOC, "null inout variable");
    }

    std::string resStr;
    SqlErrorObject errObj;
    results->extractFirstValue(resStr, errObj);
    if (errObj.isSet()) {
        return false;
    }
    try {
        *value = boost::lexical_cast<unsigned int>(resStr);
    } catch (boost::bad_lexical_cast& ex) {
        return false;
    }
    return true;
}

// Normalizes key path, takes user-provided key and converts it into
// acceptable path for storage.
std::string norm_key(std::string const& key) {
    // root key is stored as empty string (if stored at all)
    std::string path(key == "/" ? "" : key);
    return path;
}

}  // anonymous namespace

namespace lsst { namespace qserv { namespace css {

class KvTransaction : public sql::SqlTransactionScope {
public:
    using Ptr = std::shared_ptr<KvTransaction>;

    static Ptr create(sql::SqlConnection& conn) {
        return sql::SqlTransactionScope::create<KvTransaction>(conn);
    }

    ~KvTransaction() override = default;

    /// Override to throw an appropriate exception.
    void throwException(util::Issue::Context const& ctx, std::string const& msg) override {
        throw util::Issue(ctx,
                          msg + " mysql(" + std::to_string(errObj.errNo()) + " " + errObj.errMsg() + ")");
    }

    friend sql::SqlTransactionScope;

protected:
    /// Constructor - create with sql::SqlTransactionScope::create<KvTransaction>
    KvTransaction(sql::SqlConnection& conn) : sql::SqlTransactionScope(conn) {}
};

KvInterfaceImplMySql::KvInterfaceImplMySql(mysql::MySqlConfig const& mysqlConf, bool readOnly)
        : _conn(sql::SqlConnectionFactory::make(mysqlConf)), _readOnly(readOnly) {}

void KvInterfaceImplMySql::_findParentId(std::string const& childKvKey, bool* hasParent,
                                         unsigned int* parentKvId, KvTransaction const& transaction) {
    if (not transaction.isActive()) {
        throw CssError(ERR_LOC, "A transaction must active here.");
    }
    if (hasParent == NULL || parentKvId == NULL) {
        throw CssError(ERR_LOC, "null out-var ptr");
    }

    // acceptable childKvKey is "", "/child", "/child/child", but not "/"
    if (childKvKey.empty()) {
        *hasParent = false;
        return;
    }

    // Keys should always start with the delimiter and since we validate all keys created it should always be
    // there. However when looking for parent keys we do pull keys from the database, and there is some
    // possibility the values have been tampered with and the first delimiter removed. It's easy enough to
    // check for that condition here, so do a quick sanity check:
    if (childKvKey == "/" or 0 != childKvKey.find_first_of(KEY_PATH_DELIMITER)) {
        LOGS(_log, LOG_LVL_ERROR, "_findParentId - badly formatted childKvKey:" << childKvKey);
        throw CssError(ERR_LOC, "_findParentId - invalid childKvKey");
    }

    size_t loc = childKvKey.find_last_of(KEY_PATH_DELIMITER);
    std::string const parentKey(childKvKey, 0, loc);
    std::string query =
            str(boost::format("SELECT kvId FROM kvData WHERE kvKey='%1%'") % _escapeSqlString(parentKey));
    sql::SqlResults results;
    sql::SqlErrorObject errObj;
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "_findParentId - query failed: " << query);
        throw CssError(ERR_LOC, errObj);
    } else {
        // If the key is not found then the parent does not exist; create it.
        if (not extractIntValueFromSqlResults(&results, parentKvId)) {
            // _create will throw if parent can't be created.
            *parentKvId = _create(parentKey, "", false, transaction);
        }
        *hasParent = true;
    }
}

std::string KvInterfaceImplMySql::create(std::string const& key, std::string const& value, bool unique) {
    if (_readOnly) {
        throw ReadonlyCss(ERR_LOC);
    }

    // key is validated by _create
    auto transaction = KvTransaction::create(*_conn);

    std::string path = norm_key(key);
    if (unique) {
        // Need to add unique suffix which includes digits only, use 10-digit
        // numbers with padding for that. See if there are matching keys there
        // already and find highest one as initial value.

        // Key can contain characters which are special to SQL pattern matching
        // so instead of simple pattern matching we are doing pretty complicated
        // substring operations. This may kill indexing so it's not very efficient.
        const char* qTemplate =
                "SELECT RIGHT(kvKey, 10) FROM kvData WHERE "
                "LENGTH(kvKey) = %1%+10 AND LEFT(kvKey, %1%) = '%2%'";
        std::string query = (boost::format(qTemplate) % path.size() % _escapeSqlString(path)).str();

        // run query
        sql::SqlErrorObject errObj;
        sql::SqlResults results;
        LOGS(_log, LOG_LVL_DEBUG, "create - executing query: " << query);
        if (not _conn->runQuery(query, results, errObj)) {
            std::stringstream ss;
            ss << "create - " << query << " failed with err: " << errObj.errMsg() << std::ends;
            LOGS(_log, LOG_LVL_ERROR, ss.str());
            throw CssError(ERR_LOC, ss.str());
        }

        // look at results
        int uniqueId = 0;
        for (auto& row : results) {
            char* eptr;
            int val = strtol(row[0].first, &eptr, 10);
            if (*eptr == '\0') {
                // converted OK
                if (val > uniqueId) uniqueId = val;
            }
        }
        LOGS(_log, LOG_LVL_DEBUG, "create - last used unique id: " << uniqueId);

        // try to create key until succeed
        while (true) {
            ++uniqueId;
            std::ostringstream str;
            str << std::setfill('0') << std::setw(10) << uniqueId;
            path = norm_key(key) + str.str();
            try {
                _create(path, value, false, *transaction);
                break;
            } catch (KeyExistsError const& exc) {
                // exists already, try next
            }
        }

    } else {
        _create(path, value, false, *transaction);
    }

    transaction->commit();
    return path;
}

unsigned int KvInterfaceImplMySql::_create(std::string const& key, std::string const& value,
                                           bool updateIfExists, KvTransaction const& transaction) {
    if (not transaction.isActive()) {
        throw CssError(ERR_LOC, "A transaction must active here.");
    }

    _validateKey(key);

    unsigned int parentKvId(0);
    bool hasParent(false);
    _findParentId(key, &hasParent, &parentKvId, transaction);

    boost::format fmQuery;
    if (hasParent) {
        fmQuery = boost::format("INSERT INTO kvData (kvKey, kvVal, parentKvId) VALUES ('%1%', '%2%', '%3%')");
        fmQuery % _escapeSqlString(key) % _escapeSqlString(value) % parentKvId;
    } else {
        fmQuery = boost::format(
                "INSERT INTO kvData (kvKey, kvVal) VALUES ('%1%', '%2%')");  // leave parentKvId NULL
        fmQuery % _escapeSqlString(key) % _escapeSqlString(value);
    }
    if (updateIfExists) {
        fmQuery =
                boost::format("%1% ON DUPLICATE KEY UPDATE kvVal='%2%'") % fmQuery % _escapeSqlString(value);
    }
    std::string query = fmQuery.str();
    sql::SqlErrorObject errObj;
    if (not _conn->runQuery(query, errObj)) {
        switch (errObj.errNo()) {
            default:
                throw CssError(ERR_LOC, errObj);
                break;

            case ER_DUP_ENTRY:
                LOGS(_log, LOG_LVL_ERROR, "_create - SQL INSERT INTO failed: " << query);
                throw KeyExistsError(ERR_LOC, errObj);
                break;
        }
    }

    unsigned int kvId = _conn->getInsertId();
    LOGS(_log, LOG_LVL_DEBUG, "_create - executed query: " << query << ", kvId is:" << kvId);
    return kvId;
}

void KvInterfaceImplMySql::set(std::string const& key, std::string const& value) {
    if (_readOnly) {
        throw ReadonlyCss(ERR_LOC);
    }

    // key is validated by _create
    auto transaction = KvTransaction::create(*_conn);
    _create(norm_key(key), value, true, *transaction);
    transaction->commit();
}

bool KvInterfaceImplMySql::exists(std::string const& key) {
    auto transaction = KvTransaction::create(*_conn);
    std::string query =
            str(boost::format("SELECT COUNT(*) FROM kvData WHERE kvKey='%1%'") % _escapeSqlString(key));
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    LOGS(_log, LOG_LVL_DEBUG, "exists - executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        std::stringstream ss;
        ss << "exists - " << query << " failed with err: " << errObj.errMsg() << std::ends;
        LOGS(_log, LOG_LVL_ERROR, ss.str());
        throw CssError(ERR_LOC, ss.str());
    }

    unsigned int count(0);
    if (not extractIntValueFromSqlResults(&results, &count)) {
        throw CssError(ERR_LOC, "failed to extract int value from query");
    }

    if (count > 1) {
        throw CssError(ERR_LOC, "multiple keys for key");
    }
    transaction->commit();
    return 1 == count;
}

std::map<std::string, std::string> KvInterfaceImplMySql::getMany(std::vector<std::string> const& keys) {
    for (auto& key : keys) {
        _validateKey(norm_key(key));
    }

    // build query
    std::string query = "SELECT kvKey, kvVal FROM kvData WHERE kvKey IN (";
    bool first = true;
    for (auto& key : keys) {
        if (not first) query += ", ";
        first = false;
        query += '"';
        query += _escapeSqlString(norm_key(key));
        query += '"';
    }
    query += ')';

    // run query
    auto transaction = KvTransaction::create(*_conn);
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    LOGS(_log, LOG_LVL_DEBUG, "getMany - executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        std::stringstream ss;
        ss << "getMany - " << query << " failed with err: " << errObj.errMsg() << std::ends;
        LOGS(_log, LOG_LVL_ERROR, ss.str());
        throw CssError(ERR_LOC, ss.str());
    }

    // copy results
    std::map<std::string, std::string> res;
    for (auto& row : results) {
        // row is the vector of pair<char const*, unsigned long>
        // key cannot be NULL, but value could be?
        const char* key = row[0].first;
        const char* val = row[1].first ? row[1].first : "";
        res.insert(std::make_pair(key, val));
    }

    transaction->commit();
    return res;
}

std::vector<std::string> KvInterfaceImplMySql::getChildren(std::string const& parentKey) {
    std::string key = norm_key(parentKey);

    _validateKey(key);
    // get the children with a /fully/qualified/path
    auto transaction = KvTransaction::create(*_conn);
    std::vector<std::string> strVec = _getChildrenFullPath(key, *transaction);
    transaction->commit();

    // trim off the parent key, leaving only the last item in the path.
    for (std::vector<std::string>::iterator strItr = strVec.begin(); strItr != strVec.end(); ++strItr) {
        size_t loc = strItr->find_last_of(KEY_PATH_DELIMITER) + 1;
        if (strItr->size() == loc) {
            // technically this shouldn't happen because keys shoudln't end with '/', but a little safety does
            // not hurt...
            continue;
        }
        *strItr = strItr->substr(loc, strItr->size());
    }
    return strVec;
}

std::map<std::string, std::string> KvInterfaceImplMySql::getChildrenValues(std::string const& parentKey) {
    std::string key = norm_key(parentKey);

    _validateKey(key);

    // get the children with a /fully/qualified/path
    auto transaction = KvTransaction::create(*_conn);
    unsigned int parentId;
    if (not _getIdFromServer(key, &parentId, *transaction)) {
        if (not exists(key)) {
            throw NoSuchKey(ERR_LOC, parentKey);
        }
    }

    std::string query =
            str(boost::format("SELECT kvKey, kvVal FROM kvData WHERE parentKvId='%1%'") % parentId);
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    LOGS(_log, LOG_LVL_DEBUG, "getChildrenValues - executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        std::stringstream ss;
        ss << "getChildrenValues - " << query << " failed with err: " << errObj.errMsg() << std::ends;
        LOGS(_log, LOG_LVL_ERROR, ss.str());
        throw CssError(ERR_LOC, ss.str());
    }

    std::map<std::string, std::string> res;
    for (auto& row : results) {
        if (row[0].first[0] == '\0') {
            // skip root key, and this should not happen at all
            continue;
        }
        std::string key = row[0].first;

        // remove prefix up to last path delimiter
        std::string::size_type loc = key.find_last_of(KEY_PATH_DELIMITER) + 1;
        if (key.size() == loc) {
            // technically this shouldn't happen because keys shouldn't end with '/', but a little safety does
            // not hurt...
            continue;
        }
        key.erase(0, loc);

        std::string val(row[1].first ? row[1].first : "");
        res.insert(std::make_pair(key, val));
    }

    transaction->commit();

    return res;
}

std::vector<std::string> KvInterfaceImplMySql::_getChildrenFullPath(std::string const& parentKey,
                                                                    KvTransaction const& transaction) {
    if (not transaction.isActive()) {
        throw CssError(ERR_LOC, "A transaction must active here.");
    }
    _validateKey(parentKey);
    unsigned int parentId;
    if (not _getIdFromServer(parentKey, &parentId, transaction)) {
        if (not exists(parentKey)) {
            throw NoSuchKey(ERR_LOC, parentKey);
        }
    }

    std::string query = str(boost::format("SELECT kvKey FROM kvData WHERE parentKvId='%1%'") % parentId);
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    LOGS(_log, LOG_LVL_DEBUG, "_getChildrenFullPath - executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        std::stringstream ss;
        ss << "_getChildrenFullPath - " << query << " failed with err: " << errObj.errMsg() << std::ends;
        LOGS(_log, LOG_LVL_ERROR, ss.str());
        throw CssError(ERR_LOC, ss.str());
    }

    errObj.reset();
    std::vector<std::string> strVec;
    if (not results.extractFirstColumn(strVec, errObj)) {
        std::stringstream ss;
        ss << "_getChildrenFullPath - failed to extract children from " << query
           << " failed with err: " << errObj.errMsg() << std::ends;
        LOGS(_log, LOG_LVL_ERROR, ss.str());
        throw CssError(ERR_LOC, ss.str());
    }

    return strVec;
}

void KvInterfaceImplMySql::deleteKey(std::string const& keyArg) {
    if (_readOnly) {
        throw ReadonlyCss(ERR_LOC);
    }

    std::string key = norm_key(keyArg);

    auto transaction = KvTransaction::create(*_conn);
    _delete(key, *transaction);
    transaction->commit();
}

std::string KvInterfaceImplMySql::dumpKV(std::string const& key) {
    // It's better to make them ordered so that /key comes before /key/subkey
    std::string query = "SELECT kvKey, kvVal FROM kvData ORDER BY kvKey";

    // run query
    KvTransaction::Ptr transaction = KvTransaction::create(*_conn);
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    LOGS(_log, LOG_LVL_DEBUG, "dumpKV - executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        std::stringstream ss;
        ss << "dumpKV - " << query << " failed with err: " << errObj.errMsg() << std::ends;
        LOGS(_log, LOG_LVL_ERROR, ss.str());
        throw CssError(ERR_LOC, ss.str());
    }

    // copy results into property tree
    const string pfx(norm_key(key) + "/");
    ptree::ptree tree;
    for (auto& row : results) {
        // filter the key, note that root key which is empty string will
        // be filtered out because pfx is never empty
        if (boost::starts_with(row[0].first, pfx)) {
            // row is the vector of pair<char const*, unsigned long>
            // key cannot be NULL, but value could be?
            if (row[1].first == nullptr or *row[1].first == '\0') {
                tree.push_back(ptree::ptree::value_type(row[0].first, ptree::ptree()));
            } else {
                tree.push_back(ptree::ptree::value_type(row[0].first, ptree::ptree(row[1].first)));
            }
        }
    }

    transaction->commit();

    // format property tree into a string as JSON
    std::ostringstream str;
    ptree::json_parser::write_json(str, tree);
    return str.str();
}

void KvInterfaceImplMySql::_delete(std::string const& key, KvTransaction const& transaction) {
    if (not transaction.isActive()) {
        throw CssError(ERR_LOC, "A transaction must active here.");
    }
    _validateKey(key);

    // recursively delete child keys first
    std::vector<string> childKeys = _getChildrenFullPath(key, transaction);
    for (std::vector<std::string>::iterator strItr = childKeys.begin(); strItr != childKeys.end(); ++strItr) {
        _delete(*strItr, transaction);
    }

    std::string query = str(boost::format("DELETE FROM kvData WHERE kvKey='%1%'") % _escapeSqlString(key));
    sql::SqlErrorObject errObj;
    sql::SqlResults resultsObj;
    LOGS(_log, LOG_LVL_DEBUG, "deleteKey - executing query: " << query);
    if (not _conn->runQuery(query, resultsObj, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "deleteKey - " << query << " failed with err: " << errObj.errMsg());
        throw CssError(ERR_LOC, errObj);
    }

    // limit the row count to 1 - there should not be multiple rows for any key.
    auto affectedRows = resultsObj.getAffectedRows();
    if (affectedRows < 1) {
        LOGS(_log, LOG_LVL_ERROR, "deleteKey - failed (no such key) running query:" << query);
        throw NoSuchKey(ERR_LOC, errObj);
    } else if (affectedRows > 1) {
        LOGS(_log, LOG_LVL_ERROR,
             "deleteKey - failed (too many (" << affectedRows << ") rows deleted) running query: " << query);
        throw CssError(ERR_LOC, "deleteKey - unexpectedly deleted more than 1 row.");
    }
}

std::string KvInterfaceImplMySql::_get(std::string const& keyArg, std::string const& defaultValue,
                                       bool throwIfKeyNotFound) {
    std::string key = norm_key(keyArg);
    auto transaction = KvTransaction::create(*_conn);

    std::string val;
    sql::SqlErrorObject errObj;
    std::string query =
            str(boost::format("SELECT kvVal FROM kvData WHERE kvKey='%1%'") % _escapeSqlString(key));
    sql::SqlResults results;
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "_get - query failed: " << query);
        throw CssError(ERR_LOC, errObj);
    } else {
        errObj.reset();
        if (not results.extractFirstValue(val, errObj)) {
            if (throwIfKeyNotFound) {
                LOGS(_log, LOG_LVL_ERROR, "_get - error extracting value: " << errObj.errMsg());
                throw NoSuchKey(ERR_LOC, errObj);
            } else {
                val = defaultValue;
            }
        }
    }

    transaction->commit();
    return val;
}

bool KvInterfaceImplMySql::_getIdFromServer(std::string const& key, unsigned int* id,
                                            KvTransaction const& transaction) {
    if (not transaction.isActive()) {
        throw CssError(ERR_LOC, "A transaction must active here.");
    }

    std::string query =
            str(boost::format("SELECT kvId FROM kvData WHERE kvKey='%1%'") % _escapeSqlString(key));
    sql::SqlResults results;
    sql::SqlErrorObject errObj;
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "_getIdFromServer - query failed: " << query);
        throw CssError(ERR_LOC, errObj);
        return false;
    }
    errObj.reset();
    if (not extractIntValueFromSqlResults(&results, id)) {
        return false;
    }

    return true;
}

void KvInterfaceImplMySql::_validateKey(std::string const& key) {
    // There is no need for a transaction here.

    // empty key means root
    if (key.empty()) return;

    // verify that:
    // - key is less than max length
    // - first character should be a delimiter
    // - last character should not be a delimiter
    if (key.size() > KvInterface::MAX_KEY_LENGTH || key.find_first_of(KEY_PATH_DELIMITER) != 0 ||
        key.find_last_of(KEY_PATH_DELIMITER) == key.size() - 1) {
        LOGS(_log, LOG_LVL_DEBUG, "create - rejecting key: " << key);
        throw CssError(ERR_LOC, "invalid key");
    }
}

std::string KvInterfaceImplMySql::_escapeSqlString(std::string const& str) {
    // There is no need for a transaction here.

    sql::SqlErrorObject errObj;
    std::string escapedStr;
    if (not _conn->escapeString(str, escapedStr, errObj)) {
        throw CssError(ERR_LOC, errObj);
    }
    return escapedStr;
}

}}}  // namespace lsst::qserv::css
