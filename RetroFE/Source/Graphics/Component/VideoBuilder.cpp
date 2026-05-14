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

#include "VideoBuilder.h"
#include "../../Utility/Utils.h"
#include <string_view> // Added for string_view

 // 1. Optimized Extension Array (No heap allocations, evaluates at compile time)
#ifdef WIN32
static constexpr std::string_view kVidExts[] = {
    "mp4", "avi", "mkv", "mp3", "wav", "flac"
};
#else
static constexpr std::string_view kVidExts[] = {
    "mp4", "MP4", "avi", "AVI", "mkv", "MKV",
    "mp3", "MP3", "wav", "WAV", "flac", "FLAC"
};
#endif

// 2. High-Performance string builder (Exactly 1 allocation)
static inline std::string makePrefix(const std::string& path, const std::string& name) {
    std::string s;
    s.reserve(path.size() + name.size() + 2);
    s.append(path);
    if (!path.empty()) {
        const char c = path.back();
        if (c != '/' && c != '\\')
#ifdef _WIN32
            s.push_back('\\');
#else
            s.push_back('/');
#endif
    }
    s.append(name);
    return s;
}

VideoComponent* VideoBuilder::createVideo(const std::string& path, Page& page, const std::string& name,
    int monitor, int numLoops, bool softOverlay, int listId,
    const int* perspectiveCorners,
    Component* recycleTarget) {
    // 3. Fast path building
    const std::string prefix = makePrefix(path, name);

    std::string file;
    // 4. Use the iterator-based findMatchingFile to match ImageBuilder
    if (Utils::findMatchingFile(std::string_view(prefix), std::begin(kVidExts), std::end(kVidExts), file)) {
        // Attempt recycling
        if (recycleTarget && recycleTarget->recycleAsVideo(file, name)) {
            return static_cast<VideoComponent*>(recycleTarget);
        }
        // Fallback to new allocation
        return new VideoComponent(page, file, monitor, numLoops, softOverlay, listId, perspectiveCorners);
    }
    return nullptr;
}

bool VideoBuilder::resolveVideoPath(const std::string& path, const std::string& name, std::string& outFile) {
    const std::string prefix = makePrefix(path, name);
    return Utils::findMatchingFile(std::string_view(prefix), std::begin(kVidExts), std::end(kVidExts), outFile);
}

VideoComponent* VideoBuilder::createVideoFromResolved(const std::string& exactFile, const std::string& name, Page& page,
    int monitor, int numLoops, bool softOverlay, int listId,
    const int* perspectiveCorners, Component* recycleTarget) {
    if (recycleTarget && recycleTarget->recycleAsVideo(exactFile, name)) {
        return static_cast<VideoComponent*>(recycleTarget);
    }
    return new VideoComponent(page, exactFile, monitor, numLoops, softOverlay, listId, perspectiveCorners);
}