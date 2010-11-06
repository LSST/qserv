/* 
 * LSST Data Management System
 * Copyright 2008, 2009, 2010 LSST Corporation.
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
#include "lsst/qserv/master/SpatialUdfHandler.h"
 
// Pkg
#include "lsst/qserv/master/parseTreeUtil.h"

// Boost
#include <boost/make_shared.hpp>
// std
#include <algorithm>
#include <cstdlib>
#include <list>
#include <iostream>
#include <iterator>
#include <sstream>

// namespace modifiers
namespace qMaster = lsst::qserv::master;

using std::stringstream;
using boost::make_shared;

// Internal helpers
namespace {

    class strToDoubleFunc {
    public:
        double operator()(std::string const& s) { 
            char const* start = s.c_str();
            char const* eptr;
            // Cast away const. strtod won't write any chars anyway.
            double d = std::strtod(start, const_cast<char**>(&eptr));
            if(s.size() != static_cast<std::string::size_type>(eptr-start)) {
                std::stringstream s;
                s << "Exception converting string to double ("
                  << s << ")";
                throw s.str();
            }
            return d;
        }
    };
    
    // Tokenize a string delimited by ',' and place it into a container, 
    // transforming it if desired.
    template <class Container, class T>
    Container& tokenizeInto(std::string const& s, 
                            Container& c) {
        std::string delimiter(",");
        std::string::size_type pos = 0;
        std::string::size_type lastPos = 0;
        T transform;
        lastPos = s.find_first_not_of(delimiter, 0);
        while(std::string::npos != lastPos) {
            pos = s.find_first_of(delimiter, lastPos);
            std::string token(s, lastPos, pos-lastPos);
            c.push_back(transform(token));
            if(std::string::npos == pos) {
                break;
            } else {
                lastPos = s.find_first_not_of(delimiter, pos);
            }
        }
        return c;
    }
    template <typename Target>
    class coercePrint {
    public:
        coercePrint(std::ostream& o_, const char* d_) 
            : o(o_), d(d_), first(true) {}
        template<typename T>
        void operator()(T const& t) { 
            if(!first) { o << d; }
            else { first = false; }
            o << (Target)t;
        }
        std::ostream& o;
        char const* d;
        bool first;
    };

    template <class Map>
    typename Map::mapped_type const& getFromMap(Map const& m, 
                                              typename Map::key_type const& key,
                                              typename Map::mapped_type const& defValue) {
        typename Map::const_iterator i = m.find(key);
        if(i == m.end()) {
            return defValue;
        } else {
            return i->second;
        }
    }

} // anonymous namespace

////////////////////////////////////////////////////////////////////////
// SpatialUdfHandler::Restriction
////////////////////////////////////////////////////////////////////////
class qMaster::SpatialUdfHandler::Restriction {
public:
    template <class C>
    Restriction(std::string const& name, C const& c) 
        : _name(name), _params(c.size()) { 
        std::copy(c.begin(), c.end(), _params.begin()); 
        _setGenerator();
    }

    std::string getUdfCallString(StringMap const& tableConfig) {
        if(_generator.get()) {
            return (*_generator)(tableConfig);
        }
        return std::string();
    }
    class Generator {
    public:
        virtual ~Generator() {}
        virtual std::string operator()(StringMap const& tableConfig) = 0; 
    private:
    };
private:
    class ObjectIdGenerator : public Generator {
    public:
        ObjectIdGenerator(std::vector<double> const& paramNums_) 
            :  paramNums(paramNums_) {}


        virtual std::string operator()(StringMap const& tableConfig) {
            std::stringstream s;
            std::string oidStr(getFromMap<StringMap>(tableConfig,
                                                         "objectIdCol", 
                                                         "objectId"));
            s << oidStr << " IN (";
            // coerce params to integer.
            std::for_each(paramNums.begin(), paramNums.end(), 
                          coercePrint<int>(s, ","));
            s << ")";
            return s.str();
        }
        std::vector<double> const& paramNums; 
    };

    class AreaGenerator : public Generator {
    public:
        AreaGenerator(char const* fName_, int paramCount_,
                      std::vector<double> const& params_) 
            :  fName(fName_), paramCount(paramCount_), params(params_) {}
        virtual std::string operator()(StringMap const& tableConfig) {
            std::stringstream s;
            std::string raStr(getFromMap<StringMap>(tableConfig,
                                                         "raCol", 
                                                         "ra"));

            std::string declStr(getFromMap<StringMap>(tableConfig,
                                                         "declCol", 
                                                         "decl"));
            s << "qserv_" << fName << "(" << raStr << "," << declStr
              << ",";
            if(paramCount == USE_STRING) {
                s << '"'; // Place params inside a string.
                std::for_each(params.begin(), params.end(), 
                              coercePrint<double>(s," "));
                s << '"';
            } else {
                std::for_each(params.begin(), params.end(), 
                              coercePrint<double>(s,","));
                if(params.size() > 
                   static_cast<std::vector<double>::size_type>(paramCount)) {
                    throw std::string("multi not supported yet");
                }
            }
            s << ")";
            return s.str();
        }
        char const* const fName;
        const int paramCount;
        std::vector<double> const& params; 
        static const int USE_STRING = -999;
    };
    void _setGenerator();
    std::string _name;
    std::vector<double> _params;
    boost::shared_ptr<Generator> _generator;
};

void qMaster::SpatialUdfHandler::Restriction::_setGenerator() {
    if(_name == "qserv_areaspec_box") {
        _generator.reset(dynamic_cast<Generator*>
                         (new AreaGenerator("ptInSphBox", 4, _params)));
    } else if(_name == "qserv_areaspec_circle") {
        _generator.reset(dynamic_cast<Generator*>
                         (new AreaGenerator("ptInSphCircle", 3, _params)));
    } else if(_name == "qserv_areaspec_ellipse") {
        _generator.reset(dynamic_cast<Generator*>
                         (new AreaGenerator("ptInSphEllipse", 5, _params)));
    } else if(_name == "qserv_areaspec_poly") {
        _generator.reset(dynamic_cast<Generator*>
                         (new AreaGenerator("ptInSphPoly", 
                                            AreaGenerator::USE_STRING,
                                            _params)));
    } else if(_name == "qserv_objectId") {
        ObjectIdGenerator* g = new ObjectIdGenerator(_params);
        _generator.reset(dynamic_cast<Generator*>(g));
    }
}

////////////////////////////////////////////////////////////////////////
// SpatialUdfHandler::FromWhereHandler
////////////////////////////////////////////////////////////////////////
class qMaster::SpatialUdfHandler::FromWhereHandler : public VoidOneRefFunc {
public:
    FromWhereHandler(qMaster::SpatialUdfHandler& suh) : _suh(suh) {}
    virtual ~FromWhereHandler() {}
    virtual void operator()(antlr::RefAST fw) {
        if(!_suh._getIsPatched()) {
            if(_suh.getASTFactory() && !_suh.getWhereIntruder().empty()) {
                std::string intruder = "WHERE " + _suh.getWhereIntruder();
                insertTextNodeAfter(_suh.getASTFactory(), intruder, 
                                getLastSibling(fw));
            }
        } else {
            // Already patched, don't do anything.
        }
        std::cout << "fromWhere: " << walkTreeString(fw) << std::endl;
    }
private:
    qMaster::SpatialUdfHandler& _suh;
};

////////////////////////////////////////////////////////////////////////
// SpatialUdfHandler::WhereCondHandler
////////////////////////////////////////////////////////////////////////
class qMaster::SpatialUdfHandler::WhereCondHandler : public VoidOneRefFunc {
public:
    WhereCondHandler(qMaster::SpatialUdfHandler& suh) : _suh(suh) {}
    virtual ~WhereCondHandler() {}
    virtual void operator()(antlr::RefAST where) {
        // If we see a where condition, we can immediately patch it.
        if(_suh.getASTFactory()  && !_suh.getWhereIntruder().empty()) {
            std::string intruder = _suh.getWhereIntruder() + " AND";
            insertTextNodeAfter(_suh.getASTFactory(), intruder, where);
        }
        // Remember that we patched the tree.
        _suh._markAsPatched();
        //std::cout << "whereCond: " << walkTreeString(where) << std::endl;
        //std::cout << "Got limit -> " << limit << std::endl;            
    }
private:
    qMaster::SpatialUdfHandler& _suh;
};

////////////////////////////////////////////////////////////////////////
// SpatialUdfHandler::RestrictorHandler
////////////////////////////////////////////////////////////////////////
class qMaster::SpatialUdfHandler::RestrictorHandler : public VoidVoidFunc {
public:
    RestrictorHandler(qMaster::SpatialUdfHandler& suh) : _suh(suh) {}
    virtual ~RestrictorHandler() {}
    virtual void operator()() {
        
        //std::cout << "Finalizing qserv restrictor spec" << std::endl;
        _suh._setHasRestriction();
    }
private:
    qMaster::SpatialUdfHandler& _suh;
};

////////////////////////////////////////////////////////////////////////
// SpatialUdfHandler::FctSpecHandler
////////////////////////////////////////////////////////////////////////
class qMaster::SpatialUdfHandler::FctSpecHandler : public VoidTwoRefFunc {
public:
    FctSpecHandler(qMaster::SpatialUdfHandler& suh) : _suh(suh) {}
    virtual ~FctSpecHandler() {}
    virtual void operator()(antlr::RefAST name, antlr::RefAST params) {
        if(_suh._getHasRestriction()) {
            std::cout << "ERROR: conflicting restriction clauses."
                      << " Ignoring " << walkTreeString(name)
                      << std::endl;
            return;
        }
        std::string paramStrRaw = walkTreeString(params);
        std::string paramStr = paramStrRaw.substr(0, paramStrRaw.size() - 1);
        std::list<double> paramNums;

        tokenizeInto<std::list<double>, strToDoubleFunc>(paramStr, paramNums);
        boost::shared_ptr<Restriction> r(new Restriction(name->getText(),
                                                         paramNums));
        _suh._restrictions.push_back(r);
        
        // Debug printout:
        // std::cout << "Got new restrictor spec " 
        //    << name->getText() << "--";
        std::copy(paramNums.begin(), paramNums.end(), 
                  std::ostream_iterator<double>(std::cout, ","));
        std::cout << "Spec yielded " 
                  << r->getUdfCallString(_suh.getTableConfig()) <<std::endl;

        // Edit the parse tree
        collapseNodeRange(name, getLastSibling(params));
        name->setText(r->getUdfCallString(_suh.getTableConfig()));
    }
private:
    qMaster::SpatialUdfHandler& _suh;
};

////////////////////////////////////////////////////////////////////////
// SpatialUdfHandler
////////////////////////////////////////////////////////////////////////
qMaster::SpatialUdfHandler::SpatialUdfHandler(antlr::ASTFactory* factory, 
                                              StringMap const& tableConfig)
    
    : _fromWhere(new FromWhereHandler(*this)),
      _whereCond(new WhereCondHandler(*this)),
      _restrictor(new RestrictorHandler(*this)),
      _fctSpec(new FctSpecHandler(*this)),
      _isPatched(false),
      _factory(factory),
      _hasRestriction(false),
      _tableConfig(tableConfig) {
    if(!_factory) {
        std::cerr << "WARNING: SpatialUdfHandler non-functional (null factory)"
                  << std::endl;
    }
    // For testing:
    //    double dummy[] = {0.0,0.0,1,1};
    //    setExpression("box",dummy, 4);
}

void qMaster::SpatialUdfHandler::setExpression(std::string const& funcName,
                                               double* first, int nitems) {
    std::stringstream ss;
    int oneless = nitems - 1;
    ss << funcName << "(";
    if(nitems > 0) {
        for(int i=0; i < oneless; ++i) {
            ss << first[i] << ", ";
        }
        ss << first[oneless];
    }
    ss << ")";
    _whereIntruder = ss.str();
}
#if 0
// MySQL UDF signatures.  see udf/MySqlSpatialUdf.c in qserv/worker
    double qserv_angSep (ra1, dec1, ra2, dec2);
    int qserv_ptInSphBox (ra, dec, ramin, decmin, ramax,decmax);
    int qserv_ptInSphCircle (ra, dec, racenter, deccenter, radius);
    int  qserv_ptInSphEllipse (ra, dec, racenter,deccenter, smaa,smia,ang);
    int qserv_ptInSphPoly (ra, dec, ra0, poly);
    // poly = string.  "ra0 dec0 ra1 dec1 ra2 dec2 ..." 
    // space separated ra/dec pairs.
#endif
