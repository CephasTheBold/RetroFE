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

#include "TweenTypes.h"
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>
#include <cstddef>
#include <cstdint>

class ViewInfo;

class Tween {
public:
    using EasingKernel = float (*)(float, float, float);

    Tween(TweenProperty property, TweenAlgorithm type, float start, float end, float duration, const std::string& playlistFilter = "");

    // Animate using high-precision elapsed time
    float animate(double elapsedTime) const;
    float animate(double elapsedTime, float startValue) const;

    // Core animation logic using floats for performance
    static float animateSingle(TweenAlgorithm type, float start, float end, float duration, float elapsedTime);

    static TweenAlgorithm getTweenType(const std::string& name);
    static std::optional<TweenProperty> getTweenProperty(const std::string& name);
    bool matchesPlaylist(const std::string& currentPlaylist) const;
    static bool matchesPlaylistTokens(const std::vector<std::string>& tokens, const std::string& currentPlaylist);
    static uint32_t playlistTokenId(const std::string& token);
    static bool matchesPlaylistTokenIds(const std::vector<uint32_t>& tokenIds, uint32_t currentPlaylistId, bool hasCurrentPlaylist);
    TweenAlgorithm algorithm() const { return type; }
    float startValue() const { return start; }
    float endValue() const { return end; }
    static EasingKernel getKernel(TweenAlgorithm type);
    static void evaluateBatch(TweenAlgorithm type, const float* progress, const float* start, const float* change, float* out, size_t count);

    TweenProperty property;
    float  duration;
    bool   startDefined{ true };
    std::string playlistFilter;
    const std::vector<std::string>& playlistTokens() const { return playlistFilterTokens; }
    const std::vector<uint32_t>& playlistTokenIds() const { return playlistFilterTokenIds; }
    std::vector<std::string> playlistFilterTokens;
    std::vector<uint32_t> playlistFilterTokenIds;

private:
    // Easing functions use a normalized progress value for calculation.
    // p: progress (0.0 to 1.0), b: beginning value, c: change in value (end - start).
    static float linear(float p, float b, float c);
    static float easeInQuadratic(float p, float b, float c);
    static float easeOutQuadratic(float p, float b, float c);
    static float easeInOutQuadratic(float p, float b, float c);
    static float easeInCubic(float p, float b, float c);
    static float easeOutCubic(float p, float b, float c);
    static float easeInOutCubic(float p, float b, float c);
    static float easeInQuartic(float p, float b, float c);
    static float easeOutQuartic(float p, float b, float c);
    static float easeInOutQuartic(float p, float b, float c);
    static float easeInQuintic(float p, float b, float c);
    static float easeOutQuintic(float p, float b, float c);
    static float easeInOutQuintic(float p, float b, float c);
    static float easeInSine(float p, float b, float c);
    static float easeOutSine(float p, float b, float c);
    static float easeInOutSine(float p, float b, float c);
    static float easeInExponential(float p, float b, float c);
    static float easeOutExponential(float p, float b, float c);
    static float easeInOutExponential(float p, float b, float c);
    static float easeInCircular(float p, float b, float c);
    static float easeOutCircular(float p, float b, float c);
    static float easeInOutCircular(float p, float b, float c);

    static std::unordered_map<std::string, TweenAlgorithm> tweenTypeMap_;
    static std::unordered_map<std::string, TweenProperty> tweenPropertyMap_;
    static std::unordered_map<std::string, uint32_t> playlistTokenIdMap_;
    static uint32_t nextPlaylistTokenId_;

    TweenAlgorithm type;
    float start;
    float end;
};
