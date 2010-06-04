// Standard
#include <cstring>
#include <iostream>
#include <sstream>
#include <list>
#include <deque>

// Boost
#include "boost/shared_ptr.hpp"
#include "boost/format.hpp"
#include "boost/regex.hpp"

// ANTLR
#include "antlr/AST.hpp"
#include "antlr/CommonAST.hpp"

// Package
#include "lsst/qserv/master/parser.h"
#include "lsst/qserv/master/parseTreeUtil.h"
#include "lsst/qserv/master/SqlParseRunner.h"

namespace qMaster = lsst::qserv::master;
using qMaster::walkTreeString;
using qMaster::AggregateMgr;

namespace { // Anonymous

// Helper for checking strings
bool endsWith(std::string const& s, char const* ending) {
    std::string::size_type p = s.rfind(ending, std::string::npos);
    std::string::size_type l = std::strlen(ending);
    return p == (s.size() - l);
}

} // Anonymous namespace

///////////////////////////////////////////////////////////////////////////
// class Substitution
///////////////////////////////////////////////////////////////////////////
qMaster::Substitution::Substitution(std::string template_, 
                                    std::string const& delim, 
                                    bool shouldFinalize) 
    : _template(template_), _shouldFinalize(shouldFinalize) {
    _build(delim);
}
    
std::string qMaster::Substitution::transform(Mapping const& m) {
    // This can be made more efficient by pre-sizing the result buffer
    // copying directly into it, rather than creating
    // intermediate string objects and appending.
    //
    unsigned pos = 0;
    std::string result;
    // No re-allocations if transformations are constant-size.
    result.reserve(_template.size()); 

#if 0
    for(Mapping::const_iterator i = m.begin(); i != m.end(); ++i) {
	std::cout << "mapping " << i->first << " " << i->second << std::endl;
    }
#endif
    for(std::vector<Item>::const_iterator i = _index.begin();
	i != _index.end(); ++i) {
	// Copy bits since last match
	result += _template.substr(pos, i->position - pos);
	// Copy substitution
	Mapping::const_iterator s = m.find(i->name);
	if(s == m.end()) {
	    result += i->name; // passthrough.
	} else {
	    result += s->second; // perform substitution
	}
	// Update position
	pos = i->position + i->length;
    }
    // Copy remaining.
    if(pos < _template.length()) {
	result += _template.substr(pos);
    }
    return result;
}

// Let delim = ***
// blah blah ***Name*** blah blah
//           |         |
//         pos       endpos
//           |-length-|
//        name = Name
void qMaster::Substitution::_build(std::string const& delim) {
    //int maxLength = _max(names.begin(), names.end());
    int delimLength = delim.length();
    for(unsigned pos=_template.find(delim); 
	pos < _template.length(); 
	pos = _template.find(delim, pos+1)) {
	unsigned endpos = _template.find(delim, pos + delimLength);
	Item newItem;
	if(_shouldFinalize) {
	    newItem.position = pos;	
	    newItem.length = (endpos - pos) + delimLength;
	} else {
	    newItem.position = pos + delimLength;
	    newItem.length = endpos - pos - delimLength;
	}
	newItem.name.assign(_template, pos + delimLength,
			    endpos - pos - delimLength);
	// Note: length includes two delimiters.
	_index.push_back(newItem);
	pos = endpos;

	// Sanity check:
	// Check to see if the name is in names.	    
    }
}

///////////////////////////////////////////////////////////////////////////
// class SqlSubstitution
///////////////////////////////////////////////////////////////////////////
qMaster::SqlSubstitution::SqlSubstitution(std::string const& sqlStatement, 
				 Mapping const& mapping) 
    : _delimiter("*?*"), _hasAggregate(false) {
    _build(sqlStatement, mapping);
    //
}

void qMaster::SqlSubstitution::importSubChunkTables(char** cStringArr) {
    _subChunked.clear();
    for(int i=0; cStringArr[i]; ++i) {
        std::string s = cStringArr[i];
        _subChunked.push_back(s);
        if(!endsWith(s, "SelfOverlap")) {
            _subChunked.push_back(s + "SelfOverlap");
        }
        if(!endsWith(s, "FullOverlap")) {
            _subChunked.push_back(s + "FullOverlap");
        }        
    }
}
    
std::string qMaster::SqlSubstitution::transform(Mapping const& m, int chunk, 
                                       int subChunk) {
    if(!_substitution.get()) return std::string();
    return _fixDbRef(_substitution->transform(m), chunk, subChunk);
}

std::string qMaster::SqlSubstitution::substituteOnly(Mapping const& m) {
    if(!_substitution.get()) return std::string();
    return _substitution->transform(m);
}

void qMaster::SqlSubstitution::_build(std::string const& sqlStatement, 
                             Mapping const& mapping) {
    // 
    std::string template_;

    Mapping::const_iterator end = mapping.end();
    std::list<std::string> names;
    for(Mapping::const_iterator i=mapping.begin(); i != end; ++i) {
	names.push_back(i->first);
    }
    boost::shared_ptr<SqlParseRunner> spr(newSqlParseRunner(sqlStatement, 
                                                            _delimiter));
    spr->setup(names);
    if(spr->getHasAggregate()) {
	template_ = spr->getAggParseResult();
    } else {
	template_ = spr->getParseResult();
    } 
    _computeChunkLevel(spr->getHasChunks(), spr->getHasSubChunks());
    if(template_.empty()) {
	_errorMsg = spr->getError();
    } else {
        _substitution = SubstPtr(new Substitution(template_, _delimiter, true));
        _hasAggregate = spr->getHasAggregate();
        _fixupSelect = spr->getFixupSelect();
        _fixupPost = spr->getFixupPost();
    }
}


std::string qMaster::SqlSubstitution::_fixDbRef(std::string const& s, 
                                       int chunk, int subChunk) {

    // # Replace sometable_CC_SS or anything.sometable_CC_SS 
    // # with Subchunks_CC, 
    // # where CC and SS are chunk and subchunk numbers, respectively.
    // # Note that "sometable" is any subchunked table.
    std::string result = s;
    DequeConstIter e = _subChunked.end();
    for(DequeConstIter i=_subChunked.begin(); i != e; ++i) {
        std::string sName = (boost::format("%s_%d_%d") 
                             % *i % chunk % subChunk).str();
        std::string pat = (boost::format("(\\w+\\.)?%s") % sName).str();        
        boost::regex r(pat);
        std::string sub = (boost::format("Subchunks_%d.%s") 
                           % chunk % sName).str();
        //std::cout << "sName=" << sName << "  pat=" << pat << std::endl;
        result =  boost::regex_replace(result, r, sub);
        //std::cout << "out=" << result << std::endl;
    }
    return result;

    // for s in slist:
    //     sName = "%s_%d_%d" % (s, chunk, subChunk)
    //     patStr = "(\w+[.])?%s" % sName
    //     sub = "Subchunks_%d.%s" % (chunk, sName)
    //     res = re.sub(patStr, sub, res)
    // return res

}

void qMaster::SqlSubstitution::_computeChunkLevel(bool hasChunks, bool hasSubChunks) {
    // SqlParseRunner's TableList handler will know if it applied 
    // any subchunk rules, or if it detected any chunked tables.

    if(hasChunks) {
	if(hasSubChunks) {
	    _chunkLevel = 2;
	} else {
	    _chunkLevel = 1;
	}
    } else {
	_chunkLevel = 0;
    }
}

///////////////////////////////////////////////////////////////////////////
//class ChunkMapping
///////////////////////////////////////////////////////////////////////////
qMaster::ChunkMapping::Map qMaster::ChunkMapping::getMapping(int chunk, int subChunk) {
    Map m;
    ModeMap::const_iterator end = _map.end();
    std::string chunkStr = _toString(chunk);
    std::string subChunkStr = _toString(subChunk);
    static const std::string one("1");
    static const std::string two("2");
    // Insert mapping for: plainchunk, plainsubchunk1, plainsubchunk2
    for(ModeMap::const_iterator i = _map.begin(); i != end; ++i) {
	// Object --> Object_chunk
	// Object_so --> ObjectSelfOverlap_chunk
	// Object_fo --> ObjectFullOverlap_chunk
	// Object_s1 --> Object_chunk_subchunk
	// Object_s2 --> Object_chunk_subchunk
	// Object_sso --> ObjectSelfOverlap_chunk_subchunk
	// Object_sfo --> ObjectFullOverlap_chunk_subchunk
	std::string c("_" + chunkStr);
	std::string sc("_" + subChunkStr);
	std::string soc("SelfOverlap_" + chunkStr);
	std::string foc("FullOverlap_" + chunkStr);
	m.insert(MapValue(i->first, i->first + c));
	m.insert(MapValue(i->first + "_so", i->first + soc));
	m.insert(MapValue(i->first + "_fo", i->first + foc));
	if(i->second == CHUNK) {
	    // No additional work needed
	} else if (i->second == CHUNK_WITH_SUB) {
	    m.insert(MapValue(i->first + _subPrefix + one, 
			      i->first + c + sc));
	    // Might deprecate the _s2 version in this context
	    m.insert(MapValue(i->first + _subPrefix + two, 
			      i->first + c + sc));
	    m.insert(MapValue(i->first + "_sso", 
			      i->first + soc + sc));
	    m.insert(MapValue(i->first + "_sfo", 
			      i->first + foc + sc));
	}
    }
    return m;
}

qMaster::ChunkMapping::Map const& qMaster::ChunkMapping::getMapReference(int chunk, int subChunk) {
    _instanceMap = getMapping(chunk, subChunk);
    return _instanceMap;
}
