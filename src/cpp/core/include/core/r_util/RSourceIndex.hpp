/*
 * RSourceIndex.hpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#ifndef CORE_R_UTIL_R_SOURCE_INDEX_HPP
#define CORE_R_UTIL_R_SOURCE_INDEX_HPP

#include <string>
#include <vector>

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/utility.hpp>
#include <boost/regex.hpp>
#include <boost/thread/mutex.hpp>

#include <boost/algorithm/string/predicate.hpp>

#include <core/Algorithm.hpp>
#include <core/SafeConvert.hpp>
#include <core/StringUtils.hpp>
#include <core/RegexUtils.hpp>
#include <core/Thread.hpp>

#include <core/r_util/RTokenizer.hpp>

namespace core {
namespace r_util {

struct AsyncLibraryCompletions
{
   std::string package;
   std::vector<std::string> exports;
   std::vector<int> types;
   std::map< std::string, std::vector<std::string> > functions;
};

class RS4MethodParam
{
public:
   RS4MethodParam(const std::string& name, const std::string& type)
      : name_(name), type_(type)
   {
   }

   explicit RS4MethodParam(const std::string& type)
      : name_(), type_(type)
   {
   }

   // COPYING: via compiler / concrete-type

   const std::string& name() const { return name_; }
   const std::string& type() const { return type_; }

private:
   std::string name_;
   std::string type_;
};


class RSourceItem
{
public:
   enum Type
   {
      None = 0,
      Function = 1,
      Method = 2,
      Class = 3
   };

public:
   RSourceItem() : type_(None), braceLevel_(0), line_(0), column_(0)
   {
   }

   RSourceItem(int type,
               const std::string& name,
               const std::vector<RS4MethodParam>& signature,
               int braceLevel,
               std::size_t line,
               std::size_t column)
      : type_(type),
        name_(name),
        signature_(signature),
        braceLevel_(braceLevel),
        line_(line),
        column_(column)
   {
   }

   virtual ~RSourceItem() {}

   // COPYING: via compiler (copyable members)

private:
   RSourceItem(const std::string& context,
               int type,
               const std::string& name,
               const std::vector<RS4MethodParam>& signature,
               int braceLevel,
               std::size_t line,
               std::size_t column)
      : context_(context),
        type_(type),
        name_(name),
        signature_(signature),
        braceLevel_(braceLevel),
        line_(line),
        column_(column)
   {
   }

public:
   // accessors
   int type() const { return type_; }
   bool isFunction() const { return type_ == Function; }
   bool isMethod() const { return type_ == Method; }
   bool isClass() const { return type_ == Class; }
   const std::string& context() const { return context_; }
   const std::string& name() const { return name_; }
   const std::vector<RS4MethodParam>& signature() const { return signature_; }
   const int braceLevel() const { return braceLevel_; }
   int line() const { return core::safe_convert::numberTo<int>(line_,0); }
   int column() const { return core::safe_convert::numberTo<int>(column_,0); }

   // support for RSourceIndex::search

   bool nameStartsWith(const std::string& term, bool caseSensitive) const
   {
      if (caseSensitive)
         return boost::algorithm::starts_with(name_, term);
      else
         return boost::algorithm::istarts_with(name_, term);
   }

   bool nameIsSubsequence(const std::string& term, bool caseSensitive) const
   {
      return string_utils::isSubsequence(name_, term, !caseSensitive);
   }

   bool nameContains(const std::string& term, bool caseSensitive) const
   {
      if (caseSensitive)
         return boost::algorithm::contains(name_, term);
      else
         return boost::algorithm::icontains(name_, term);
   }

   bool nameMatches(const boost::regex& regex,
                    bool prefixOnly,
                    bool caseSensitive) const
   {
      return regex_utils::textMatches(name_, regex, prefixOnly, caseSensitive);
   }

   RSourceItem withContext(const std::string& context) const
   {
      return RSourceItem(context,
                         type_,
                         name_,
                         signature_,
                         braceLevel_,
                         line_,
                         column_);
   }

private:
   std::string context_;
   int type_;
   std::string name_;
   std::vector<RS4MethodParam> signature_;
   int braceLevel_;
   std::size_t line_;
   std::size_t column_;
};


class RSourceIndex : boost::noncopyable
{
public:
   // Index the provided R source code so that we can efficiently search
   // for functions within it
   //
   // Requirements for code:
   //   - Must be UTF-8 encoded
   //   - Must use \n only for linebreaks
   //
   RSourceIndex(const std::string& context,
                const std::string& code);

   const std::string& context() const { return context_; }

   template <typename OutputIterator>
   OutputIterator search(
                  const std::string& newContext,
                  const boost::function<bool(const RSourceItem&)> predicate,
                  OutputIterator out) const
   {
      // perform the copy and transform to include context
      core::algorithm::copy_transformed_if(
                items_.begin(),
                items_.end(),
                out,
                predicate,
                boost::bind(&RSourceItem::withContext, _1, newContext));

      // return the output iterator
      return out;
   }

   template <typename OutputIterator>
   OutputIterator search(
                  const boost::function<bool(const RSourceItem&)> predicate,
                  OutputIterator out) const
   {
      return search(context_, predicate, out);
   }

   template <typename OutputIterator>
   OutputIterator search(const std::string& term,
                         const std::string& newContext,
                         bool prefixOnly,
                         bool caseSensitive,
                         OutputIterator out) const
   {
      // define the predicate
      boost::function<bool(const RSourceItem&)> predicate;

      // check for wildcard character
      if (term.find('*') != std::string::npos)
      {
         boost::regex patternRegex = regex_utils::wildcardPatternToRegex(
                                                caseSensitive ?
                                                    term :
                                                    string_utils::toLower(term));
         predicate = boost::bind(&RSourceItem::nameMatches,
                                    _1,
                                    patternRegex,
                                    prefixOnly,
                                    caseSensitive);
      }
      else
      {
         if (prefixOnly)
            predicate = boost::bind(&RSourceItem::nameStartsWith,
                                       _1, term, caseSensitive);
         else
            predicate = boost::bind(&RSourceItem::nameIsSubsequence,
                                       _1, term, caseSensitive);
      }

      return search(newContext, predicate, out);
   }

   template <typename OutputIterator>
   OutputIterator search(const std::string& term,
                         bool prefixOnly,
                         bool caseSensitive,
                         OutputIterator out) const
   {
      return search(term, context_, prefixOnly, caseSensitive, out);
   }

private:

   // NOTE: All source indexes share a set of completions
   static std::map<std::string, AsyncLibraryCompletions>& completions()
   {
      static std::map<std::string, AsyncLibraryCompletions> s_completions;
      return s_completions;
   }

public:

   static std::set<std::string> getInferredPackages()
   {
      LOCK_MUTEX(mutex_)
      {
         return inferredPkgNames_;
      }
      END_LOCK_MUTEX
      return std::set<std::string>();
   }
   
   static void addCompletions(const std::string& package,
                              const AsyncLibraryCompletions& asyncCompletions)
   {
      LOCK_MUTEX(mutex_)
      {
         completions()[package] = asyncCompletions;
      }
      END_LOCK_MUTEX
   }
   
   static AsyncLibraryCompletions getCompletions(const std::string& package)
   {
      LOCK_MUTEX(mutex_)
      {
         return completions()[package];
      }
      END_LOCK_MUTEX
      return AsyncLibraryCompletions();
   }

   static std::vector<std::string> getUnindexedPackages()
   {
      std::vector<std::string> result;
      LOCK_MUTEX(mutex_)
      {
         for (std::set<std::string>::const_iterator it = inferredPkgNames_.begin();
              it != inferredPkgNames_.end();
              ++it)
         {
            if (completions().count(*it) == 0)
               result.push_back(*it);
         }
      }
      END_LOCK_MUTEX
      return result;
   }
   
private:
   std::string context_;
   std::vector<RSourceItem> items_;
   
   // private fields related to the current set of library completions
   static std::set<std::string> inferredPkgNames_;
   static boost::mutex mutex_;
   
};


} // namespace r_util
} // namespace core 


#endif // CORE_R_UTIL_R_SOURCE_INDEX_HPP

