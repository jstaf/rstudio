/*
 * SessionThemes.cpp
 *
 * Copyright (C) 2018 by RStudio, Inc.
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

#include "SessionThemes.hpp"

#include <boost/bind.hpp>

#include <core/Error.hpp>
#include <core/Exec.hpp>
#include <core/FilePath.hpp>
#include <core/json/JsonRpc.hpp>
#include <core/system/System.hpp>

#include <core/http/Request.hpp>
#include <core/http/Response.hpp>

#include <session/SessionModuleContext.hpp>

#include <r/RRoutines.hpp>
#include <r/RSexp.hpp>

#include <fstream>
#include <map>
#include <regex>
#include <string>

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace themes {

namespace {

const std::string kDefaultThemeLocation = "/theme/default/";
const std::string kGlobalCustomThemeLocation = "/theme/custom/global/";
const std::string kLocalCustomThemeLocation = "/theme/custom/local/";

// A map from the name of the theme to the location of the file and a boolean representing
// whether or not the theme is dark.
typedef std::map<std::string, std::pair<std::string, bool> > ThemeMap;

bool strIsTrue(const std::string& toConvert)
{
   return std::regex_match(
            std::string(toConvert),
            std::regex("(?:1|t(?:rue)?))", std::regex_constants::icase));
}

bool strIsFalse(const std::string& toConvert)
{
   return std::regex_match(
            std::string(toConvert),
            std::regex("(?:0|f(?:alse)?))", std::regex_constants::icase));
}

/**
 * @brief Gets themes in the specified location.
 *
 * @param location         The location in which to look for themes.
 * @param themeMap         The map which will contain all found themes after the call.
 * @param urlPrefix        The URL prefix for the theme. Must end with "/"
 */
void getThemesInLocation(
      const rstudio::core::FilePath& location,
      ThemeMap& themeMap,
      const std::string& urlPrefix = "")
{
   using rstudio::core::FilePath;
   if (location.isDirectory())
   {
      std::vector<FilePath> locationChildren;
      location.children(&locationChildren);
      for (const FilePath& themeFile: locationChildren)
      {
         if (themeFile.hasExtensionLowerCase(".rstheme"))
         {
            const std::string k_themeFileStr = themeFile.canonicalPath();
            std::ifstream themeIFStream(k_themeFileStr);
            std::string themeContents(
               (std::istreambuf_iterator<char>(themeIFStream)),
               (std::istreambuf_iterator<char>()));
            themeIFStream.close();

            std::smatch matches;
            std::regex_search(
               themeContents,
               matches,
               std::regex("rs-theme-name\\s*:\\s*([^\\*]+?)\\s*(?:\\*|$)"));

            // If there's no name specified,use the name of the file
            std::string name;
            if (matches.size() < 2)
            {
               name = themeFile.stem();
            }
            else
            {
               // If there's at least one name specified, get the first one.
               name = matches[1];
            }

            // Find out if the theme is dark or not.
            std::regex_search(
                     themeContents,
                     matches,
                     std::regex("rs-theme-is-dark\\s*:\\s*([^\\*]+?)\\s*(?:\\*|$)"));

            bool isDark = false;
            if (matches.size() >= 2)
            {
               isDark = strIsTrue(matches[1]);
            }
            if ((matches.size() < 2) ||
                (!isDark &&
                !strIsFalse(matches[1])))
            {
               // TODO: warning / logging about using default isDark value.
            }

            themeMap[name] = std::tuple<std::string, bool>(urlPrefix + themeFile.filename(), isDark);
         }
      }
   }
}

FilePath getDefaultThemePath()
{
   return session::options().rResourcesPath().childPath("themes");
}

FilePath getGlobalCustomThemePath()
{
   using rstudio::core::FilePath;

   const char* kGlobalPathAlt = std::getenv("RS_THEME_GLOBAL_HOME");
   if (kGlobalPathAlt)
   {
      return FilePath(kGlobalPathAlt);
   }

#ifdef _WIN32
   return core::system::systemSettingsPath("RStudio\\themes", false);
#else
   return FilePath("/etc/rstudio/themes/");
#endif
}

FilePath getLocalCustomThemePath()
{
   using rstudio::core::FilePath;
   const char* kLocalPathAlt = std::getenv("RS_THEME_LOCAL_HOME");
   if (kLocalPathAlt)
   {
      return FilePath(kLocalPathAlt);
   }

#ifdef _WIN32
   return core::system::userHomePath().childPath(".R\\rstudio\\themes");
#else
   return core::system::userHomePath().childPath(".R/rstudio/themes/");
#endif
}

void getAllThemes(ThemeMap& themeMap)
{
   // Intentionally get global themes before getting user specific themes so that user specific
   // themes will override global ones.
   getThemesInLocation(getDefaultThemePath(), themeMap, kDefaultThemeLocation);
   getThemesInLocation(getGlobalCustomThemePath(), themeMap, kGlobalCustomThemeLocation);
   getThemesInLocation(getLocalCustomThemePath(), themeMap, kLocalCustomThemeLocation);
}

/**
 * @brief Gets the list of all RStudio editor themes.
 *
 * @return The list of all RStudio editor themes.
 */
SEXP rs_getThemes()
{
   ThemeMap themeMap;
   getAllThemes(themeMap);

   // Convert to an R list.
   rstudio::r::sexp::Protect protect;
   rstudio::r::sexp::ListBuilder themeListBuilder(&protect);

   for (auto theme: themeMap)
   {
      rstudio::r::sexp::ListBuilder themeDetailsListBuilder(&protect);
      themeDetailsListBuilder.add("url", theme.second.first);
      themeDetailsListBuilder.add("isDark", theme.second.second);

      themeListBuilder.add(theme.first, themeDetailsListBuilder);
   }

   return rstudio::r::sexp::create(themeListBuilder, &protect);
}

FilePath getDefaultTheme(const http::Request& request)
{
   std::string isDarkStr = request.queryParamValue("dark");
   bool isDark = strIsTrue(isDarkStr);
   if (!isDark && !strIsFalse(isDarkStr))
   {
      // TODO: Error/warning/logging
      // Note that this should be considered an internal error, since the request is generated
      // by the client and without user input.
   }

   if (isDark)
   {
      return getDefaultThemePath().childPath("tomorrow_night.rstheme");
   }
   else
   {
      return getDefaultThemePath().childPath("textmate.rstheme");
   }
}

void handleDefaultThemeRequest(const http::Request& request,
                                     http::Response* pResponse)
{
   std::string fileName = http::util::pathAfterPrefix(request, kDefaultThemeLocation);
   pResponse->setCacheableFile(getDefaultThemePath().childPath(fileName), request);
   pResponse->setContentType("text/css");
}

void handleGlobalCustomThemeRequest(const http::Request& request,
                                          http::Response* pResponse)
{
   // Note: we probably want to return a warning code instead of success so the client has the
   // ability to pop up a warning dialog or something to the user.
   std::string fileName = http::util::pathAfterPrefix(request, kGlobalCustomThemeLocation);
   FilePath requestedTheme = getLocalCustomThemePath().childPath(fileName);
   pResponse->setCacheableFile(
      requestedTheme.exists() ? requestedTheme : getDefaultTheme(request),
      request);
   pResponse->setContentType("text/css");
}

void handleLocalCustomThemeRequest(const http::Request& request,
                                         http::Response* pResponse)
{
   // Note: we probably want to return a warning code instead of success so the client has the
   // ability to pop up a warning dialog or something to the user.
   std::string fileName = http::util::pathAfterPrefix(request, kLocalCustomThemeLocation);
   FilePath requestedTheme = getLocalCustomThemePath().childPath(fileName);
   pResponse->setCacheableFile(
      requestedTheme.exists() ? requestedTheme : getDefaultTheme(request),
      request);
   pResponse->setContentType("text/css");
}

/**
 * Gets the list of all the avialble themes for the client.
 */
Error getThemes(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   ThemeMap themes;
   getAllThemes(themes);

   // Convert the theme to a json array.
   json::Array jsonThemeArray;
   for (auto theme: themes)
   {
      json::Object jsonTheme;
      jsonTheme["name"] = theme.first;
      jsonTheme["url"] = theme.second.first;
      jsonTheme["isDark"] = theme.second.second;
      jsonThemeArray.push_back(jsonTheme);
   }

   pResponse->setResult(jsonThemeArray);
   return Success();
}

} // anonymous namespace

Error initialize()
{
   using boost::bind;
   using namespace module_context;

   RS_REGISTER_CALL_METHOD(rs_getThemes, 0);

   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(sourceModuleRFile, "SessionThemes.R"))
      (bind(registerRpcMethod, "get_themes", getThemes))
      (bind(registerUriHandler, kDefaultThemeLocation, handleDefaultThemeRequest))
      (bind(registerUriHandler, kGlobalCustomThemeLocation, handleGlobalCustomThemeRequest))
      (bind(registerUriHandler, kLocalCustomThemeLocation, handleLocalCustomThemeRequest));

   return initBlock.execute();
}

} // namespace themes
} // namespace modules
} // namespace session
} // namespace rstudio