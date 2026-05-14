/* This file is part of RetroFE.
*
* RetroFE is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* RetroFE is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <list>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <memory>
#include <type_traits>

#ifdef WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#ifdef __APPLE__
struct PathHash {
    auto operator()(const std::filesystem::path& p) const noexcept {
        return std::filesystem::hash_value(p);
    }
};
#endif

class Utils
{
public:
    using FileSet = std::unordered_set<std::string>;
    static std::string replace(std::string subject, const std::string_view& search,
        const std::string_view& replace);

    static float convertFloat(const std::string_view& content);
    static int convertInt(const std::string_view& content);
    static void replaceSlashesWithUnderscores(std::string& content);
#ifdef WIN32    
    static void postMessage(LPCTSTR windowTitle, UINT Msg, WPARAM wParam, LPARAM lParam);
#endif
    static std::string wstringToString(const std::wstring wstr);
    static std::string getDirectory(const std::string& filePath);
    static std::string getParentDirectory(std::string filePath);
    static std::string getEnvVar(std::string const& key);
    static void setEnvVar(const std::string& var, const std::string& value);
    static std::string getFileName(const std::string& filePath);
    static bool findMatchingFile(const std::string& prefix, const std::vector<std::string>& extensions, std::string& file);
    static bool findMatchingFile(std::string_view prefixNoExt, const std::string_view* extsBegin, const std::string_view* extsEnd, std::string& outPath);
    static void coarseSleep(double seconds_to_sleep);
    static void preciseSleep(double seconds_to_sleep);
    static std::string toLower(const std::string& inputStr);
    static std::string uppercaseFirst(std::string str);
    static std::string filterComments(const std::string& line);
    static std::string trimEnds(const std::string& str);
    static void listToVector(const std::string& str, std::vector<std::string>& vec, char delimiter);
    static int gcd(int a, int b);
    static std::string trim(std::string str);
    static bool isAbsolutePath(const std::string& path);
    static bool isSubPath(const std::string& candidate);
    static std::string removeAbsolutePath(const std::string& fullPath);
    static bool isOutputATerminal();
    static bool startsWith(const std::string& fullString, const std::string& startOfString);
    static bool startsWithAndStrip(std::string& fullString, const std::string& startOfString);
    static std::string getOSType();
    static std::string obfuscate(const std::string& data);
    static std::string deobfuscate(const std::string& data);
    static std::string removeNullCharacters(const std::string& input);
	static size_t getMemoryUsage();

    struct PathPart {
        std::string_view view;
        std::string storage;

        template <typename T>
        PathPart(const T& val) {
            if constexpr (std::is_same_v<std::decay_t<T>, std::filesystem::path>) {
                storage = val.string();
                view = storage;
            }
            else {
                view = std::string_view(val);
            }
        }
    };

    // 2. The highly optimized combinePath function
    template <typename... Paths>
    static std::string combinePath(const Paths&... paths) {
        if constexpr (sizeof...(paths) == 0) {
            return std::string();
        }
        else {
            // Unpack all arguments into our helper structs (does the conversion exactly once per path object)
            PathPart parts[] = { PathPart(paths)... };

            // Calculate exact memory needed (sum of lengths + room for slashes)
            size_t totalLen = sizeof...(paths);
            for (const auto& part : parts) {
                totalLen += part.view.size();
            }

            std::string result;
            result.reserve(totalLen); // EXACTLY ONE ALLOCATION

            // Append parts cleanly
            for (const auto& part : parts) {
                std::string_view v = part.view;
                if (v.empty()) continue;

                if (!result.empty()) {
                    char last = result.back();
                    // Add separator if missing
                    if (last != '/' && last != '\\') {
#ifdef _WIN32
                        result.push_back('\\');
#else
                        result.push_back('/');
#endif
                    }
                    // Skip leading slashes on the new part to avoid double-slashes
                    size_t start = 0;
                    while (start < v.size() && (v[start] == '/' || v[start] == '\\')) {
                        start++;
                    }
                    result.append(v.substr(start));
                }
                else {
                    // First element, keep leading slashes (e.g., root Unix directories)
                    result.append(v);
                }
            }

            return result;
        }
    }

#ifdef WIN32
    static const char pathSeparator = '\\';
#else
    static const char pathSeparator = '/';
#endif

private:
#ifdef __APPLE__
    static std::unordered_map<std::filesystem::path, std::shared_ptr<const FileSet>, PathHash> fileCache;
    static std::unordered_set<std::filesystem::path, PathHash> nonExistingDirectories;
#else
    static std::unordered_map<std::filesystem::path, std::shared_ptr<const FileSet>> fileCache;
    static std::unordered_set<std::filesystem::path> nonExistingDirectories;
#endif

    // NEW: guard cache structures (safe even if you currently run single-threaded)
    static std::shared_mutex fileCacheMutex;

    // NEW: normalize directory keys (fixes "ghost cache" on Windows)
    static std::filesystem::path normalizeCacheKey(const std::filesystem::path& p);

    // NEW: ensure cache entry exists (or negative cached), then return file-set pointer
    static std::shared_ptr<const FileSet> getOrPopulateFileSet(const std::filesystem::path& directory);
    static void populateCache(const std::filesystem::path& directory);
    static const std::string obfuscationKey; // Key for XOR obfuscation
    static std::string xorOperation(const std::string& data, const std::string& key);


    Utils();
    virtual ~Utils();
};
