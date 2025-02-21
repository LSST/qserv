/*
 * LSST Data Management System
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
#ifndef LSST_QSERV_HTTP_REQUESTBODYJSON_H
#define LSST_QSERV_HTTP_REQUESTBODYJSON_H

// System headers
#include <algorithm>
#include <stdexcept>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// This header declarations
namespace lsst::qserv::http {

// TODO:UJ This should be renamed RequestBodyJson, coding standards.

/**
 * Class RequestBodyJSON represents the request body parsed into a JSON object.
 * This type of an object is only available for requests that have the following
 * header: 'Content-Type: application/json'.
 */
class RequestBodyJSON {
public:
    /// parsed body of the request
    nlohmann::json objJson = nlohmann::json::object();

    RequestBodyJSON() = default;
    RequestBodyJSON(RequestBodyJSON const&) = default;
    RequestBodyJSON& operator=(RequestBodyJSON const&) = default;

    ~RequestBodyJSON() = default;

    /// Make a new RequestBody based on `js`
    /// TODO:UJ This would be much more efficient if this class had objJson defined as
    ///   a const reference or pointer to const, but implementation is likely ugly.
    RequestBodyJSON(nlohmann::json const& js) : objJson(js) {}

    /**
     * Check if the specified parameter is present in the input JSON object.
     * @param obj  JSON object to be inspected.
     * @param name  The name of a parameter.
     * @return  'true' if the parameter was found.
     * @throw invalid_argument  If the input structure is not the valid JSON object.
     */
    bool has(nlohmann::json const& obj, std::string const& name) const;

    /**
     * Check if thw specified parameter is present in the body.
     * @param name  The name of a parameter.
     * @return  'true' if the parameter was found.
     * @throw invalid_argument  If the body is not the valid JSON object.
     */
    bool has(std::string const& name) const;

    /**
     * The helper method for finding and returning a value of a required parameter.
     * @param obj  JSON object to be inspected.
     * @param name  The name of a parameter.
     * @return  A value of the parameter.
     * @throw invalid_argument  If the input structure is not the valid JSON object,
     *   or if the parameter wasn't found.
     */
    template <typename T>
    static T required(nlohmann::json const& obj, std::string const& name) {
        if (not obj.is_object()) {
            throw std::invalid_argument("RequestBodyJSON::" + std::string(__func__) +
                                        "<T>[static] parameter 'obj' is not a valid JSON object");
        }

        if (auto const iter = obj.find(name); iter != obj.end()) {
            return *iter;
        }
        throw std::invalid_argument("RequestBody::" + std::string(__func__) +
                                    "<T>[static] required parameter " + name +
                                    " is missing in the request body");
    }

    /**
     * Find and return a value of a required parameter.
     * @param name  The name of a parameter.
     * @return  A value of the parameter.
     * @throw invalid_argument  If the parameter wasn't found.
     */
    template <typename T>
    T required(std::string const& name) const {
        return required<T>(objJson, name);
    }

    // The following methods are used to extract the values of the parameters from the JSON object
    // where they could be stored as a string or as a number. The methods will try to convert the
    // value to the desired type if it's a string.
    // The methods will throw an exception if the parameter wasn't found, or if its value
    // is not an integer.

    unsigned int requiredUInt(std::string const& name) const;
    unsigned int optionalUInt(std::string const& name, unsigned int defaultValue = 0) const;

    int requiredInt(std::string const& name) const;
    int optionalInt(std::string const& name, int defaultValue = 0) const;

    /**
     * Return a value of a required parameter. Also ensure that the value is permitted.
     * @param name  The name of a parameter.
     * @param permitted  A collection of values to be checked against.
     * @return  A value of the parameter.
     * @throw invalid_argument  If the parameter wasn't found, or if its value
     *   is not in a collection of the permitted values.
     */
    template <typename T>
    T required(std::string const& name, std::vector<T> const& permitted) const {
        auto const value = required<T>(objJson, name);
        if (_in(value, permitted)) return value;
        throw std::invalid_argument("RequestBodyJSON::" + std::string(__func__) +
                                    "<T>(permitted) a value of parameter " + name + " is not allowed.");
    }

    /**
     * Find and return a value for the specified optional parameter.
     * @param name  The name of a parameter.
     * @param defaultValue  A value to be returned if the parameter wasn't found.
     * @return  A value of the parameter or the default value.
     */
    template <typename T>
    T optional(std::string const& name, T const& defaultValue) const {
        if (objJson.find(name) != objJson.end()) return objJson[name];
        return defaultValue;
    }

    /**
     * Return a value of an optional parameter. Also ensure that the value is permitted.
     * @note the default value must be also in a set of the permitted values.
     * @param name  The name of a parameter.
     * @param defaultValue  A value to be returned if the parameter wasn't found.
     * @param permitted  A collection of values to be checked against.
     * @return  A value of the parameter or the default value.
     */
    template <typename T>
    T optional(std::string const& name, T const& defaultValue, std::vector<T> const& permitted) const {
        auto const value = optional<T>(name, defaultValue);
        if (_in(value, permitted)) return value;
        throw std::invalid_argument("RequestBodyJSON::" + std::string(__func__) +
                                    "<T>(permitted) a value of parameter " + name + " is not allowed.");
    }

    /**
     * Find and return a vector of values for the specified required parameter.
     * @param name  The name of a parameter.
     * @return  A collection of the values for the parameter.
     * @throw invalid_argument  If the parameter wasn't found.
     */
    template <typename T>
    std::vector<T> requiredColl(std::string const& name) const {
        auto const itr = objJson.find(name);
        if (itr == objJson.end()) {
            throw std::invalid_argument("RequestBodyJSON::" + std::string(__func__) +
                                        "<T> required parameter " + name + " is missing in the request body");
        }
        if (not itr->is_array()) {
            throw std::invalid_argument("RequestBodyJSON::" + std::string(__func__) +
                                        "<T> a value of the required parameter " + name + " is not an array");
        }
        std::vector<T> coll;
        for (size_t i = 0, size = itr->size(); i < size; ++i) {
            T val = (*itr)[i];
            coll.push_back(val);
        }
        return coll;
    }

    /**
     * Find and return a vector of values for the specified optional parameter.
     * @param name  The name of a parameter.
     * @param defaultValue  A value to be returned if the parameter wasn't found.
     * @return  A value of the parameter or the default value.
     */
    template <typename T>
    std::vector<T> optionalColl(std::string const& name, std::vector<T> const& defaultValue) const {
        auto const itr = objJson.find(name);
        if (itr == objJson.end()) return defaultValue;
        return requiredColl<T>(name);
    }

private:
    /**
     * Check if the specified value is found in a collection of permitted values.
     * @param value  A value to be checked.
     * @param permitted  A collection of values to be checked against.
     * @return  'true' if the collection is empty or if the input value is found in the collection.
     */
    template <typename T>
    static bool _in(T const& value, std::vector<T> const& permitted) {
        return permitted.empty() or
               std::find(permitted.cbegin(), permitted.cend(), value) != permitted.cend();
    }

    /**
     * The helper method for finding and returning a value of a required parameter.
     * @param func  The name of the calling context.
     * @param name  The name of a parameter.
     * @return  A value of the parameter.
     * @throw std::invalid_argument  If the parameter wasn't found.
     */
    nlohmann::json _get(std::string const& func, std::string const& name) const;
};

}  // namespace lsst::qserv::http

#endif  // LSST_QSERV_HTTP_REQUESTBODYJSON_H
