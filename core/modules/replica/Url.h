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
#ifndef LSST_QSERV_URL_H
#define LSST_QSERV_URL_H

// System headers
#include <string>

// This header declarations
namespace lsst {
namespace qserv {
namespace replica {

/**
 * Class Url is a helper class for parsing and validating URLs.
 */
class Url {
public:
    /// Types of resources
    enum Scheme {
        FILE, HTTP, HTTPS
    };

    // Default construction is prohibited to avoid extra complexity in managing
    // a "valid" state of the resource object.
    Url() = delete;

    Url(Url const&) = default;
    Url& operator=(Url const&) = default;

    ~Url() = default;

    /**
     * @param url A resource string to be parsed and validated.
     * @throw std::invalid_argument If the string is empty, is too short or based on
     *   a non-supported scheme.
     */
    explicit Url(std::string const& url);

    Scheme scheme() const { return _scheme; }
    std::string const& url() const { return _url; }

    /**
     * @return A file path part of a url based on Scheme::FILE
     * @throw std::logic_error If attempting to call for non-file urls.
     */
    std::string const& filePath() const;

private:
    /**
     * The helper method for generating error messages.
     * @param func A client context the metgod is called.
     * @param msg An optional message to be appended by the output
     * @return A string which starts with a scope and includes the optional message.
     */
    static std::string _error(std::string const& func,
                              std::string const& msg=std::string());

    /**
     * Translate and validate the URL stored in attribute _url.
     * @throw std::invalid_argument If the string is empty, is too short or based on
     *   a non-supported scheme.
     */
    void _translate();

    // Input parameters
    std::string _url;

    // Cached state
    Scheme _scheme = Scheme::FILE;
    std::string _filePath;   ///< local path to a file (FILE scheme only)
};

}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_URL_H
