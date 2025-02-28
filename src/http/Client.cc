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

// Class header
#include "http/Client.h"

// Qserv headers
#include "http/ClientConnPool.h"
#include "http/Exceptions.h"

// Standard headers
#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace std;
using json = nlohmann::json;

namespace lsst::qserv::http {

size_t forwardToClient(char* ptr, size_t size, size_t nmemb, void* client) {
    size_t const nchars = size * nmemb;
    reinterpret_cast<Client*>(client)->_store(ptr, nchars);
    return nchars;
}

Client::Client(http::Method method, string const& url, string const& data, vector<string> const& headers,
               ClientConfig const& clientConfig, shared_ptr<ClientConnPool> const& connPool)
        : _method(method),
          _url(url),
          _data(data),
          _headers(headers),
          _clientConfig(clientConfig),
          _connPool(connPool) {
    if (_method == http::Method::MIMEPOST) {
        throw invalid_argument("http::Client::" + string(__func__) +
                               ":  method MIMEPOST is not allowed in this constructor");
    }
    _hcurl = curl_easy_init();
    assert(_hcurl != nullptr);
}

Client::Client(string const& url, list<ClientMimeEntry> const& mimeData, vector<string> const& headers,
               ClientConfig const& clientConfig, shared_ptr<ClientConnPool> const& connPool)
        : _method(http::Method::MIMEPOST),
          _url(url),
          _mimeData(mimeData),
          _headers(headers),
          _clientConfig(clientConfig),
          _connPool(connPool) {
    _hcurl = curl_easy_init();
    assert(_hcurl != nullptr);
}

Client::~Client() {
    curl_slist_free_all(_hlist);
    curl_easy_cleanup(_hcurl);
    curl_mime_free(_form);
}

void Client::read(CallbackType const& onDataRead) {
    string const context = "http::Client::" + string(__func__) + ": ";
    if (onDataRead == nullptr) {
        throw invalid_argument(context + "no callback provided");
    }
    _onDataRead = onDataRead;

    _setConnOptions();
    _setSslCertOptions();
    _setProxyOptions();

    _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_URL)",
                          curl_easy_setopt(_hcurl, CURLOPT_URL, _url.c_str()));
    _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_CUSTOMREQUEST)",
                          curl_easy_setopt(_hcurl, CURLOPT_CUSTOMREQUEST, nullptr));
    if (_method == http::Method::MIMEPOST) {
        if (_mimeData.empty()) {
            throw invalid_argument(context + "no data provided for MIMEPOST");
        }
        curl_mime_free(_form);
        _form = curl_mime_init(_hcurl);
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_MIMEPOST)",
                              curl_easy_setopt(_hcurl, CURLOPT_MIMEPOST, _form));
        for (auto const& mimeEntry : _mimeData) {
            curl_mimepart* field = curl_mime_addpart(_form);
            _curlEasyErrorChecked("curl_mime_name", curl_mime_name(field, mimeEntry.name.c_str()));
            if (mimeEntry.value.empty() == mimeEntry.filename.empty()) {
                throw invalid_argument(context + "invalid JSON data provided for MIMEPOST");
            }
            if (!mimeEntry.value.empty()) {
                _curlEasyErrorChecked("curl_mime_data",
                                      curl_mime_data(field, mimeEntry.value.c_str(), CURL_ZERO_TERMINATED));
            } else {
                _curlEasyErrorChecked("curl_mime_filename",
                                      curl_mime_filename(field, mimeEntry.filename.c_str()));
                _curlEasyErrorChecked("curl_mime_filedata",
                                      curl_mime_filedata(field, mimeEntry.filename.c_str()));
            }
            if (!mimeEntry.type.empty()) {
                _curlEasyErrorChecked("curl_mime_type", curl_mime_type(field, mimeEntry.type.c_str()));
            }
        }
    } else {
        if (_method == http::Method::GET) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_HTTPGET)",
                                  curl_easy_setopt(_hcurl, CURLOPT_HTTPGET, 1L));
        } else if (_method == http::Method::POST) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_POST)",
                                  curl_easy_setopt(_hcurl, CURLOPT_POST, 1L));
        } else {
            _curlEasyErrorChecked(
                    "curl_easy_setopt(CURLOPT_CUSTOMREQUEST)",
                    curl_easy_setopt(_hcurl, CURLOPT_CUSTOMREQUEST, http::method2string(_method).data()));
        }
        if (!_data.empty()) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_POSTFIELDS)",
                                  curl_easy_setopt(_hcurl, CURLOPT_POSTFIELDS, _data.c_str()));
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_POSTFIELDSIZE)",
                                  curl_easy_setopt(_hcurl, CURLOPT_POSTFIELDSIZE, _data.size()));
        }
    }
    curl_slist_free_all(_hlist);
    _hlist = nullptr;
    for (auto& header : _headers) {
        _hlist = curl_slist_append(_hlist, header.c_str());
    }
    _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_HTTPHEADER)",
                          curl_easy_setopt(_hcurl, CURLOPT_HTTPHEADER, _hlist));
    _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_FAILONERROR)",
                          curl_easy_setopt(_hcurl, CURLOPT_FAILONERROR, 1L));
    _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_WRITEFUNCTION)",
                          curl_easy_setopt(_hcurl, CURLOPT_WRITEFUNCTION, forwardToClient));
    _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_WRITEDATA)",
                          curl_easy_setopt(_hcurl, CURLOPT_WRITEDATA, this));
    _curlEasyErrorChecked("curl_easy_perform()", curl_easy_perform(_hcurl));
}

json Client::readAsJson() {
    vector<char> data;
    this->read([&data](char const* buf, size_t size) { data.insert(data.cend(), buf, buf + size); });
    return json::parse(data);
}

void Client::_setConnOptions() {
    if (_clientConfig.httpVersion != CURL_HTTP_VERSION_NONE) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_HTTP_VERSION)",
                              curl_easy_setopt(_hcurl, CURLOPT_HTTP_VERSION, _clientConfig.httpVersion));
    }
    if (_clientConfig.bufferSize > 0) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_BUFFERSIZE)",
                              curl_easy_setopt(_hcurl, CURLOPT_BUFFERSIZE, _clientConfig.bufferSize));
    }
    if (_clientConfig.connectTimeout > 0) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_CONNECTTIMEOUT)",
                              curl_easy_setopt(_hcurl, CURLOPT_CONNECTTIMEOUT, _clientConfig.connectTimeout));
    }
    if (_clientConfig.timeout > 0) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_TIMEOUT)",
                              curl_easy_setopt(_hcurl, CURLOPT_TIMEOUT, _clientConfig.timeout));
    }
    if (_clientConfig.lowSpeedLimit > 0) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_LOW_SPEED_LIMIT)",
                              curl_easy_setopt(_hcurl, CURLOPT_LOW_SPEED_LIMIT, _clientConfig.lowSpeedLimit));
    }
    if (_clientConfig.lowSpeedTime > 0) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_LOW_SPEED_TIME)",
                              curl_easy_setopt(_hcurl, CURLOPT_LOW_SPEED_TIME, _clientConfig.lowSpeedTime));
    }
    if (_clientConfig.tcpKeepAlive) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_TCP_KEEPALIVE)",
                              curl_easy_setopt(_hcurl, CURLOPT_TCP_KEEPALIVE, 1L));
        if (_clientConfig.tcpKeepIdle > 0) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_TCP_KEEPIDLE)",
                                  curl_easy_setopt(_hcurl, CURLOPT_TCP_KEEPIDLE, _clientConfig.tcpKeepIdle));
        }
        if (_clientConfig.tcpKeepIntvl > 0) {
            _curlEasyErrorChecked(
                    "curl_easy_setopt(CURLOPT_TCP_KEEPINTVL)",
                    curl_easy_setopt(_hcurl, CURLOPT_TCP_KEEPINTVL, _clientConfig.tcpKeepIntvl));
        }
    }
    if (_connPool != nullptr) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_SHARE)",
                              curl_easy_setopt(_hcurl, CURLOPT_SHARE, _connPool->shareCurl()));
        if (_connPool->maxConnections() > 0) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_MAXCONNECTS)",
                                  curl_easy_setopt(_hcurl, CURLOPT_MAXCONNECTS, _connPool->maxConnections()));
        }
    }
}

void Client::_setSslCertOptions() {
    if (!_clientConfig.sslVerifyHost) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_SSL_VERIFYHOST)",
                              curl_easy_setopt(_hcurl, CURLOPT_SSL_VERIFYHOST, 0L));
    }
    if (_clientConfig.sslVerifyPeer) {
        if (!_clientConfig.caPath.empty()) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_CAPATH)",
                                  curl_easy_setopt(_hcurl, CURLOPT_CAPATH, _clientConfig.caPath.c_str()));
        }
        if (!_clientConfig.caInfo.empty()) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_CAINFO)",
                                  curl_easy_setopt(_hcurl, CURLOPT_CAINFO, _clientConfig.caInfo.c_str()));
        }
    } else {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_SSL_VERIFYPEER)",
                              curl_easy_setopt(_hcurl, CURLOPT_SSL_VERIFYPEER, 0L));
    }
}

void Client::_setProxyOptions() {
    if (!_clientConfig.proxy.empty()) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_PROXY)",
                              curl_easy_setopt(_hcurl, CURLOPT_PROXY, _clientConfig.proxy.c_str()));
        if (_clientConfig.httpProxyTunnel != 0) {
            _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_HTTPPROXYTUNNEL)",
                                  curl_easy_setopt(_hcurl, CURLOPT_HTTPPROXYTUNNEL, 1L));
        }
    }
    if (!_clientConfig.noProxy.empty()) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_NOPROXY)",
                              curl_easy_setopt(_hcurl, CURLOPT_NOPROXY, _clientConfig.noProxy.c_str()));
    }
    if (!_clientConfig.proxySslVerifyHost) {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_PROXY_SSL_VERIFYHOST)",
                              curl_easy_setopt(_hcurl, CURLOPT_PROXY_SSL_VERIFYHOST, 0L));
    }
    if (_clientConfig.proxySslVerifyPeer) {
        if (!_clientConfig.proxyCaPath.empty()) {
            _curlEasyErrorChecked(
                    "curl_easy_setopt(CURLOPT_PROXY_CAPATH)",
                    curl_easy_setopt(_hcurl, CURLOPT_PROXY_CAPATH, _clientConfig.proxyCaPath.c_str()));
        }
        if (!_clientConfig.proxyCaInfo.empty()) {
            _curlEasyErrorChecked(
                    "curl_easy_setopt(CURLOPT_PROXY_CAINFO)",
                    curl_easy_setopt(_hcurl, CURLOPT_PROXY_CAINFO, _clientConfig.proxyCaInfo.c_str()));
        }
    } else {
        _curlEasyErrorChecked("curl_easy_setopt(CURLOPT_PROXY_SSL_VERIFYPEER)",
                              curl_easy_setopt(_hcurl, CURLOPT_PROXY_SSL_VERIFYPEER, 0L));
    }
}

void Client::_curlEasyErrorChecked(string const& scope, CURLcode errnum) {
    if (errnum != CURLE_OK) {
        string errorStr = curl_easy_strerror(errnum);
        long httpResponseCode = 0;
        if (errnum == CURLE_HTTP_RETURNED_ERROR) {
            errorStr += " (on HTTP error codes 400 or greater)";
            curl_easy_getinfo(_hcurl, CURLINFO_RESPONSE_CODE, &httpResponseCode);
        }
        http::raiseRetryAllowedError(scope, " error: '" + errorStr + "', errnum: " + to_string(errnum),
                                     httpResponseCode);
    }
}

void Client::_store(char const* ptr, size_t nchars) { _onDataRead(ptr, nchars); }

}  // namespace lsst::qserv::http
