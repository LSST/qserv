/*
 * LSST Data Management System
 * Copyright 2017 LSST Corporation.
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
#ifndef LSST_QSERV_REPLICA_APPLICATION_TYPES_H
#define LSST_QSERV_REPLICA_APPLICATION_TYPES_H

/**
 * ApplicationTypes.h declares types which are used in an implementation
 * of class Application. These types are put into this header to avoid
 * cluttered the host's class header with too many details.
 */

// System headers
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Third party headers
#include <boost/lexical_cast.hpp>

// Qserv headers
#include "util/Issue.h"

// This header declarations

namespace lsst {
namespace qserv {
namespace replica {
namespace detail {

/**
 * Class ParserError represents exceptions throw by the command-line parser
 * during ptocessing arguments as per user requested syntax description.
 */
class ParserError
    :   public util::Issue {
public:
    ParserError(util::Issue::Context const& ctx,
                std::string const& message);
};

/**
 * The very base class which represents subjects which are parsed:
 * parameters, options and flags.
 */
class ArgumentParser {

public:

    /**
     * @return
     *   'true' if the specified value belongs to a collection
     *
     * @param val
     *   the value to be evaluated
     *
     * @param col
     *   the collection of allowed values
     */
    template <typename T>
    static bool in(T const& val,
                  std::vector<T> const& col) {
        return col.end() != std::find(col.begin(), col.end(), val);
    }

     // Default construction and copy semantics are prohibited

    ArgumentParser() = delete;
    ArgumentParser(ArgumentParser const&) = delete;
    ArgumentParser& operator=(ArgumentParser const&) = delete;

    /**
     * Construct the object
     *
     * @param name
     *   the name of the parameter as it will be shown in error messages
     *   (should there be any problem with parsing a value of the parameter)
     *   and the 'help' printout (if the one is requested in the constructor
     *   of the class)
     *
     * @param description
     *   the description of the parameter as it will be shown in the 'help'
     *   printout (if the one is requested in the constructor of the class)
     */
    ArgumentParser(std::string const& name,
                   std::string const& description)
    :   _name(name),
        _description(description) {
    }

    virtual ~ArgumentParser() = default;

    // Trivial accessors

    std::string const& name() const { return _name; }
    std::string const& description() const { return _description; }

    /**
     * Let a subclass to parse the input string into a value of the corresponidng
     * (as defined by the class) type.
     *
     * @param inStr - (optional) input string to be parsed
     * @throws ParserError if the text can't be parsed
     */
    virtual void parse(std::string const& inStr="") = 0;

    /**
     * Default values are supposed to be captured from user-defined variables
     * at a time when the argument objects are constructed. They're used for
     * generating a documentation.
     *
     * @return
     *   string representation of the default value of an argument
     */
    virtual std::string defaultValue() const = 0;

    /**
     * Dump the name of an argument and its value into an output stream
     *
     * @param
     *   an output stream object
     */
    virtual void dumpNameValue(std::ostream& os) const = 0;

private:
    
    // Parameters of the object passed via the class's constructor

    std::string const _name;
    std::string const _description;
};

/**
 * Dump a string representation of the argument name and its value
 * to the stream.
 */
std::ostream& operator<<(std::ostream& os, ArgumentParser const& arg) {
    arg.dumpNameValue(os);
    return os;
}

/**
 * The class representing (mandatory or optional) parameters
 */
template <typename T>
class ParameterParser
    :   public ArgumentParser {

public:

     // Default construction and copy semantics are prohibited

    ParameterParser() = delete;
    ParameterParser(ParameterParser const&) = delete;
    ParameterParser& operator=(ParameterParser const&) = delete;

    /**
     * Construct the object
     *
     * @param name
     *   the name of the parameter as it will be shown in error messages
     *   (should there be any problem with parsing a value of the parameter)
     *   and the 'help' printout (if the one is requested in the constructor
     *   of the class)
     *
     * @param description
     *   the description of the parameter as it will be shown in the 'help'
     *   printout (if the one is requested in the constructor of the class)
     *
     * @param var
     *   the reference to the corresponding variable to be initialized with
     *   a value of the paameter after successful parsing. The type of the
     *   parameter is determined by the template argument.
     *
     * @see ArgumentParser::ArgumentParser()
     */
    ParameterParser(std::string const& name,
                    std::string const& description,
                    T& var,
                    std::vector<T> const& allowedValues)
    :   ArgumentParser(name,
                       description),
        _var(var),
        _defaultValue(var),
        _allowedValues(allowedValues) {
    }

    virtual ~ParameterParser() = default;

    /**
     * @see ArgumentParser::parse()
     */
    void parse(std::string const& inStr) final {
        try {
            _var = boost::lexical_cast<T>(inStr);
        } catch (boost::bad_lexical_cast const& ex) {
            throw ParserError(
                    ERR_LOC,
                    "failed to parse a value of parameter '" + name() +
                    " from '" + inStr);
        }
        if (not _allowedValues.empty()) {
            if (not in(_var, _allowedValues)) {
                throw ParserError(
                    ERR_LOC,
                    "the value of parameter '" + name() + "' is disallowed: '" + inStr + "'");
            }
        }
    }

    /**
     * @see ArgumentParser::defaultValue()
     */
    std::string defaultValue() const final {
        std::ostringstream os;
        os << _defaultValue;
        return os.str();
    }

    /**
     * @see ArgumentParser::dumpNameValue()
     */
    void dumpNameValue(std::ostream& os) const final {
        os << name() << "=" << _var;
    }

private:
    
    /// A reference to a user variable to be initialized
    T& _var;
    
    /// A copy of the variable is captured here
    T const _defaultValue;

    /// A collection of allowed values (if provided)
    std::vector<T> const _allowedValues;
};

/**
 * The class representing named options
 */
template <typename T>
class OptionParser
    :   public ArgumentParser {

public:

     // Default construction and copy semantics are prohibited

    OptionParser() = delete;
    OptionParser(OptionParser const&) = delete;
    OptionParser& operator=(OptionParser const&) = delete;

    /**
     * Construct the object
     *
     * @param name
     *   the name of the parameter as it will be shown in error messages
     *   (should there be any problem with parsing a value of the parameter)
     *   and the 'help' printout (if the one is requested in the constructor
     *   of the class)
     *
     * @param description
     *   the description of the parameter as it will be shown in the 'help'
     *   printout (if the one is requested in the constructor of the class)
     *
     * @param var
     *   the reference to the corresponding variable to be initialized with
     *   a value of the paameter after successful parsing. The type of the
     *   parameter is determined by the template argument.
     *
     * @see ArgumentParser::ArgumentParser()
     */
    OptionParser(std::string const& name,
                 std::string const& description,
                 T& var)
    :   ArgumentParser(name,
                       description),
        _var(var),
        _defaultValue(var) {
    }

    virtual ~OptionParser() = default;

    /**
     * @see ArgumentParser::parse()
     */
    void parse(std::string const& inStr) final {
        if (inStr.empty()) return;
        try {
            _var = boost::lexical_cast<T>(inStr);
        } catch (boost::bad_lexical_cast const& ex) {
            throw ParserError(ERR_LOC,
                              "failed to parse a value of option '" + name() + " from '" + inStr);
        }
    }

    /**
     * @see ArgumentParser::defaultValue()
     */
    std::string defaultValue() const final {
        std::ostringstream os;
        os << _defaultValue;
        return os.str();
    }

    /**
     * @see ArgumentParser::dumpNameValue()
     */
    void dumpNameValue(std::ostream& os) const final {
        os << name() << "=" << _var;
    }

private:
    
    /// A reference to a user variable to be initialized
    T& _var;

    /// A copy of the variable is captured here
    T const _defaultValue;
};

/**
 * The class representing named flags
 */
class FlagParser
    :   public ArgumentParser {

public:

     // Default construction and copy semantics are prohibited

    FlagParser() = delete;
    FlagParser(FlagParser const&) = delete;
    FlagParser& operator=(FlagParser const&) = delete;

    /**
     * Construct the object
     * 
     * @param name
     *   the name of the parameter as it will be shown in error messages
     *   (should there be any problem with parsing a value of the parameter)
     *   and the 'help' printout (if the one is requested in the constructor
     *   of the class)
     *
     * @param description
     *   the description of the parameter as it will be shown in the 'help'
     *   printout (if the one is requested in the constructor of the class)
     *
     * @param var
     *   the reference to the corresponding variable to be initialized with
     *   a value of the paameter after successful parsing. The type of the
     *   parameter is determined by the template argument.
     *
     * @see ArgumentParser::ArgumentParser()
     */
    FlagParser(std::string const& name,
               std::string const& description,
               bool& var)
    :   ArgumentParser(name,
                       description),
        _var(var) {
    }

    virtual ~FlagParser() = default;

    /**
     * @see ArgumentParser::parse()
     */
    void parse(std::string const& inStr) final { _var = true; }

    /**
     * @see ArgumentParser::defaultValue()
     */
    std::string defaultValue() const final { return "false"; }

    /**
     * @see ArgumentParser::dumpNameValue()
     */
    void dumpNameValue(std::ostream& os) const final {
        os << name() << "=" << (_var ? "1" : "0");
    }

private:
    
    /// A reference to a user variable to be initialized
    bool& _var;
};


/**
 * The class for parsing command line parameters and filling variables
 * provided by a user.
 */
class Parser {

public:
    
    enum Status {

        /// The initial state for the completion code. It's used to determine
        /// if any parsing attempt has been made.
        UNDEFINED = -1,

        /// The normal completion status
        SUCCESS = 0,

        /// This status is reported after intercepting flag "--help" and printing
        /// the documentation.
        HELP_REQUESTED = 1,

        /// The status is used to report any problem with parsing arguments.
        PARSING_FAILED = 2
    };
    
     // Default construction and copy semantics are prohibited

    Parser() = delete;
    Parser(Parser const&) = delete;
    Parser& operator=(Parser const&) = delete;

    virtual ~Parser() = default;

    /**
     * Construct and initialize the parser
     *
     * @param arc              - argument count
     * @parav argv             - vector of argument values
     * @param description      - descripton of an application
     */
    Parser(int argc,
           const char* const argv[],
           std::string const& description);

    /**
     * Reset the state of the object to the one it was constructed. This
     * means that all effects of the below defined 'add' and 'parse' methods
     * will be eliminated.
     *
     * IMPORTANT: the operation will NOT return user variables mentioned in
     * the 'add' methods back to their states if method 'parse' has already
     * been called. It's up to a user to reset those variables back to the
     * desired state if teir intent behind calling this ('reset') method is
     * to reconfigure the parser and start over.
     *
     * @see method Parser::reset()
     */
    void reset();

    /**
     * Register a mandatory positional parameter for parsing. Positional
     * parameters are lined up based on an order in which the positional
     * parameter methods (this and 'optional') are
     * a being called.
     *
     * @see method Parser::optional()
     *
     * @param name
     *   the name of the parameter as it will be shown in error messages
     *   (should there be any problem with parsing a value of the parameter)
     *   and the 'help' printout (if the one is requested in the constructor
     *   of the class)
     *
     * @param description
     *   the description of the parameter as it will be shown in the 'help'
     *   printout (if the one is requested in the constructor of the class)
     *
     * @param var
     *   the reference to the corresponding variable to be initialized with
     *   a value of the paameter after successful parsing. The type of the
     *   parameter is determined by the template argument.
     *
     * @param allowedValues
     *   (optional) collection of allowed values of the parameter.
     *
     * @throws std::invalid_argument
     *   if the name of the argument is empty, or if another parameter, option
     *   or flag under the same name was already requested earlier.
     *
     * @return
     *   a reference to the parser object in order to allow chained calls
     */
    template <typename T>
    Parser& required(std::string const& name,
                              std::string const& description,
                              T& var,
                              std::vector<T> const& allowedValues = std::vector<T>()) {
        registerArgument(name);
        _required.push_back(
            std::move(
                std::make_unique<ParameterParser<T>>(
                    name,
                    description,
                    var,
                    allowedValues
                )
            )
        );
        return *this;
    }

    /**
     * Register an optinal positional parameter for parsing. The original
     * state of a variable passed into the method will assumed as the default
     * value of teh parameter. The value will stay intact if the parameter
     * won't be found in a command line. Otherwise this method is similar to
     * the above defined 'required'.
     *
     * @see method Parser::required()
     * @return
     *   a reference to the parser object in order to allow chained calls
     */
    template <typename T>
    Parser& optional(std::string const& name,
                              std::string const& description ,
                              T& var,
                              std::vector<T> const& allowedValues = std::vector<T>()) {
        registerArgument(name);
        _optional.push_back(
            std::move(
                std::make_unique<ParameterParser<T>>(
                    name,
                    description,
                    var,
                    allowedValues
                )
            )
        );
        return *this;
    }

    /**
     * Register a named option which has a value. The method is similar to
     * the above defined 'required' except it may
     * show up at any position in the command line.
     *
     * @see method Parser::optional()
     * @return
     *   a reference to the parser object in order to allow chained calls
     */
    template <typename T>
    Parser& option(std::string const& name,
                   std::string const& description,
                   T& var) {
        registerArgument(name);
        _options.emplace(
            std::make_pair(
                name,
                std::move(
                    std::make_unique<OptionParser<T>>(
                        name,
                        description,
                        var
                    )
                )
            )
        );
        return *this;
    }

    /**
     * Register a named flag. If the flag will be found among the command
     * line parameters then the variable will be set to 'true'. Otherwise
     * it will be set to 'false'. Other parameters of the method are similar
     * to the ones of the above defined 'add' methods.
     *
     * @see method Parser::option()
     * @return
     *   a reference to the parser object in order to allow chained calls
     */
    Parser& flag(std::string const& name,
                 std::string const& description,
                 bool& var);
    
    /**
     * Parse parameters, options and flags requested by above
     * defined 'add' methods. The method will return one the following
     * codes defined by Status.
     *
     * IMPORTANT: after completion (successful or not) the states of
     * some (or all) variables mentioned in the above defined methods
     * will change. It's up to a user to reset them back if the parser
     * will get reset (using method 'reset') and reconfigured.
     *
     * @see Parser::Status
     * @see method Parser::reset()
     *
     * @return completion code
     *
     * @throws ParserError for any problems occuring during the parsing
     */
    int parse();

    /**
     * @return serialize names and values of the parsed arguments serialized
     * into a string
     *
     * @throws std::logic_error if called before attempted to parse
     * th command line parameters, or if the parsing didn't successfully
     * finish with Status::SUCCESS.
     */
    std::string serializeArguments() const;

private:

    /**
     * Verify and register the name of an argument (parameter, option or flag)
     * in a dictionary to ensure that its name is unique, and no other argument
     * has been register earlier under the same name.
     *
     * @name
     *   the name of an argument
     */
    void registerArgument(std::string const& name);

    /**
     * @return the "Usage" string to be reported in case if any problem
     * with parsing the command line arguments will be seen. The current
     * implementation of this method will build and cache the string the
     * first time the metghod is invoked.
     */
    std::string const& usage();

    /**
     * @return the complete documentation to be returned if flag "--help"
     * is passed as an argument.  The current implementation of this method
     * will build and cache the string the first time the metghod is invoked
     * regardless if flag "--help" is registered or not for the application.
     */
    std::string const& help();
    
    /**
     * Read the input string and produce an output one with words
     * wrapped at wite spaces not to exceed the specified maximum width
     * of each line.
     *
     * @param str
     *   the input string to be wrapped
     *   
     * @param indent
     *   the indent at the begining of each line
     *   
     * @param width
     *   the maximum width of each line (including the apecified indent)
     *
     * @return
     *   the wrapped text, in which each line (including the last one)
     *   ends with the newline symbol.
     */
    static std::string wrap(std::string const& str,
                            std::string const& indent = "      ",
                            size_t width = 72);

private:

    // Parameters of the object

    int const _argc;
    const char* const* _argv;
    std::string const _description;

    /// The names of all parameters, options or flags are registered here
    /// to prevent duplicates.
    std::set<std::string> _allArguments;

    /// A sequence of the mandatory parameters
    std::vector<std::unique_ptr<ArgumentParser>> _required;
    
    /// A sequence of the optional parameters
    std::vector<std::unique_ptr<ArgumentParser>> _optional;

    /// A set of named options
    std::map<std::string, std::unique_ptr<ArgumentParser>> _options;

    /// A set of named flags
    std::map<std::string, std::unique_ptr<ArgumentParser>> _flags;

    /// Status code set after parsing the arguments. It's also used to avoid
    /// invoking method Parser::parse() more than one time. The default value
    /// indicates that the parser has never attempted.
    int _code;

    /// Flag which is used to trigger the "help" regime of the parser
    bool _helpFlag;

    /// The "Usage" string is build when all arguments are registered
    /// and method 'parse()' is invoked.
    std::string _usage;

    /// The documentation (invoked with "--help") string is build when all
    /// arguments are registered and method 'parse()' is invoked.
    std::string _help;
};

}}}} // namespace lsst::qserv::replica::detail

#endif // LSST_QSERV_REPLICA_APPLICATION_TYPES_H

