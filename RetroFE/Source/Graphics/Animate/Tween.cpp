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
#include "Tween.h"
#include "../../Utility/Log.h"
#include <algorithm>
#define _USE_MATH_DEFINES
#include <math.h>
#include <string>
#include <optional>
#include <sstream>
#include <mutex>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace {
std::string trimPlaylistToken(const std::string& token) {
    const size_t first = token.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return "";
    }

    const size_t last = token.find_last_not_of(" \t\n\r");
    return token.substr(first, last - first + 1);
}

#if defined(__SSE2__)
inline void evaluateLinearSse(const float* progress, const float* start, const float* change, float* out, size_t count) {
    constexpr size_t kWidth = 4;
    size_t i = 0;
    for (; i + kWidth <= count; i += kWidth) {
        const __m128 p = _mm_loadu_ps(progress + i);
        const __m128 b = _mm_loadu_ps(start + i);
        const __m128 c = _mm_loadu_ps(change + i);
        const __m128 result = _mm_add_ps(_mm_mul_ps(c, p), b);
        _mm_storeu_ps(out + i, result);
    }

    for (; i < count; ++i) {
        out[i] = change[i] * progress[i] + start[i];
    }
}

inline void evaluateEaseInQuadraticSse(const float* progress, const float* start, const float* change, float* out, size_t count) {
    constexpr size_t kWidth = 4;
    size_t i = 0;
    for (; i + kWidth <= count; i += kWidth) {
        const __m128 p = _mm_loadu_ps(progress + i);
        const __m128 b = _mm_loadu_ps(start + i);
        const __m128 c = _mm_loadu_ps(change + i);
        const __m128 p2 = _mm_mul_ps(p, p);
        const __m128 result = _mm_add_ps(_mm_mul_ps(c, p2), b);
        _mm_storeu_ps(out + i, result);
    }

    for (; i < count; ++i) {
        out[i] = change[i] * progress[i] * progress[i] + start[i];
    }
}

inline void evaluateEaseOutQuadraticSse(const float* progress, const float* start, const float* change, float* out, size_t count) {
    constexpr size_t kWidth = 4;
    const __m128 two = _mm_set1_ps(2.0f);
    size_t i = 0;
    for (; i + kWidth <= count; i += kWidth) {
        const __m128 p = _mm_loadu_ps(progress + i);
        const __m128 b = _mm_loadu_ps(start + i);
        const __m128 c = _mm_loadu_ps(change + i);
        const __m128 pTerm = _mm_sub_ps(two, p); // 2 - p
        const __m128 result = _mm_add_ps(_mm_mul_ps(c, _mm_mul_ps(p, pTerm)), b);
        _mm_storeu_ps(out + i, result);
    }

    for (; i < count; ++i) {
        out[i] = change[i] * progress[i] * (2.0f - progress[i]) + start[i];
    }
}

inline void evaluateEaseInCubicSse(const float* progress, const float* start, const float* change, float* out, size_t count) {
    constexpr size_t kWidth = 4;
    size_t i = 0;
    for (; i + kWidth <= count; i += kWidth) {
        const __m128 p = _mm_loadu_ps(progress + i);
        const __m128 b = _mm_loadu_ps(start + i);
        const __m128 c = _mm_loadu_ps(change + i);
        const __m128 p2 = _mm_mul_ps(p, p);
        const __m128 p3 = _mm_mul_ps(p2, p);
        const __m128 result = _mm_add_ps(_mm_mul_ps(c, p3), b);
        _mm_storeu_ps(out + i, result);
    }

    for (; i < count; ++i) {
        out[i] = change[i] * progress[i] * progress[i] * progress[i] + start[i];
    }
}

inline void evaluateEaseOutCubicSse(const float* progress, const float* start, const float* change, float* out, size_t count) {
    constexpr size_t kWidth = 4;
    const __m128 one = _mm_set1_ps(1.0f);
    size_t i = 0;
    for (; i + kWidth <= count; i += kWidth) {
        const __m128 p = _mm_loadu_ps(progress + i);
        const __m128 b = _mm_loadu_ps(start + i);
        const __m128 c = _mm_loadu_ps(change + i);
        const __m128 q = _mm_sub_ps(p, one);
        const __m128 q2 = _mm_mul_ps(q, q);
        const __m128 q3 = _mm_mul_ps(q2, q);
        const __m128 result = _mm_add_ps(_mm_mul_ps(c, _mm_add_ps(q3, one)), b);
        _mm_storeu_ps(out + i, result);
    }

    for (; i < count; ++i) {
        const float q = progress[i] - 1.0f;
        out[i] = change[i] * (q * q * q + 1.0f) + start[i];
    }
}
#endif
}

std::unordered_map<std::string, TweenAlgorithm> Tween::tweenTypeMap_ = {
    {"easeinquadratic", EASE_IN_QUADRATIC},
    {"easeoutquadratic", EASE_OUT_QUADRATIC},
    {"easeinoutquadratic", EASE_INOUT_QUADRATIC},
    {"easeincubic", EASE_IN_CUBIC},
    {"easeoutcubic", EASE_OUT_CUBIC},
    {"easeinoutcubic", EASE_INOUT_CUBIC},
    {"easeinquartic", EASE_IN_QUARTIC},
    {"easeoutquartic", EASE_OUT_QUARTIC},
    {"easeinoutquartic", EASE_INOUT_QUARTIC},
    {"easeinquintic", EASE_IN_QUINTIC},
    {"easeoutquintic", EASE_OUT_QUINTIC},
    {"easeinoutquintic", EASE_INOUT_QUINTIC}, // <-- Corrected "easeonoutquintic" typo
    {"easeinsine", EASE_IN_SINE},
    {"easeoutsine", EASE_OUT_SINE},
    {"easeinoutsine", EASE_INOUT_SINE},
    {"easeinexponential", EASE_IN_EXPONENTIAL},
    {"easeoutexponential", EASE_OUT_EXPONENTIAL},
    {"easeinoutexponential", EASE_INOUT_EXPONENTIAL},
    {"easeincircular", EASE_IN_CIRCULAR},
    {"easeoutcircular", EASE_OUT_CIRCULAR},
    {"easeinoutcircular", EASE_INOUT_CIRCULAR},
    {"linear", LINEAR}
};

std::unordered_map<std::string, TweenProperty> Tween::tweenPropertyMap_ = {
    {"x", TWEEN_PROPERTY_X},
    {"y", TWEEN_PROPERTY_Y},
    {"angle", TWEEN_PROPERTY_ANGLE},
    {"alpha", TWEEN_PROPERTY_ALPHA},
    {"width", TWEEN_PROPERTY_WIDTH},
    {"height", TWEEN_PROPERTY_HEIGHT},
    {"xorigin", TWEEN_PROPERTY_X_ORIGIN},
    {"yorigin", TWEEN_PROPERTY_Y_ORIGIN},
    {"xoffset", TWEEN_PROPERTY_X_OFFSET},
    {"yoffset", TWEEN_PROPERTY_Y_OFFSET},
    {"fontsize", TWEEN_PROPERTY_FONT_SIZE},
    {"backgroundalpha", TWEEN_PROPERTY_BACKGROUND_ALPHA},
    {"maxwidth", TWEEN_PROPERTY_MAX_WIDTH},
    {"maxheight", TWEEN_PROPERTY_MAX_HEIGHT},
    {"layer", TWEEN_PROPERTY_LAYER},
    {"containerx", TWEEN_PROPERTY_CONTAINER_X},
    {"containery", TWEEN_PROPERTY_CONTAINER_Y},
    {"containerwidth", TWEEN_PROPERTY_CONTAINER_WIDTH},
    {"containerheight", TWEEN_PROPERTY_CONTAINER_HEIGHT},
    {"volume", TWEEN_PROPERTY_VOLUME},
    {"nop", TWEEN_PROPERTY_NOP},
    {"restart", TWEEN_PROPERTY_RESTART}
};

std::unordered_map<std::string, uint32_t> Tween::playlistTokenIdMap_ = {};
uint32_t Tween::nextPlaylistTokenId_ = 1;


Tween::Tween(TweenProperty property, TweenAlgorithm type, float start, float end, float duration, const std::string& playlistFilter)
    : property(property)
    , duration(duration)
    , playlistFilter(playlistFilter)
    , type(type)
    , start(start)
    , end(end) {
    if (!playlistFilter.empty()) {
        std::stringstream ss(playlistFilter);
        std::string playlist;
        while (std::getline(ss, playlist, ',')) {
            playlist = trimPlaylistToken(playlist);
            if (!playlist.empty()) {
                playlistFilterTokens.push_back(playlist);
                playlistFilterTokenIds.push_back(playlistTokenId(playlist));
            }
        }
    }
}

bool Tween::matchesPlaylistTokens(const std::vector<std::string>& tokens, const std::string& currentPlaylist) {
    if (tokens.empty() || currentPlaylist.empty()) {
        return true;
    }

    return std::find(tokens.begin(), tokens.end(), currentPlaylist) != tokens.end();
}

uint32_t Tween::playlistTokenId(const std::string& token) {
    static std::mutex playlistTokenIdMapMutex;
    std::lock_guard<std::mutex> lock(playlistTokenIdMapMutex);

    auto it = playlistTokenIdMap_.find(token);
    if (it != playlistTokenIdMap_.end()) {
        return it->second;
    }

    const uint32_t assignedId = nextPlaylistTokenId_++;
    playlistTokenIdMap_[token] = assignedId;
    return assignedId;
}

bool Tween::matchesPlaylistTokenIds(const std::vector<uint32_t>& tokenIds, uint32_t currentPlaylistId, bool hasCurrentPlaylist) {
    if (tokenIds.empty() || !hasCurrentPlaylist) {
        return true;
    }

    return std::find(tokenIds.begin(), tokenIds.end(), currentPlaylistId) != tokenIds.end();
}

bool Tween::matchesPlaylist(const std::string& currentPlaylist) const {
    if (playlistFilterTokenIds.empty() || currentPlaylist.empty()) {
        return true;
    }

    return matchesPlaylistTokenIds(playlistFilterTokenIds, playlistTokenId(currentPlaylist), true);
}

std::optional<TweenProperty> Tween::getTweenProperty(const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = tweenPropertyMap_.find(key);
    if (it != tweenPropertyMap_.end()) {
        return it->second;
    }

    return std::nullopt;
}

TweenAlgorithm Tween::getTweenType(const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = tweenTypeMap_.find(key);
    if (it != tweenTypeMap_.end())
        return it->second;

    return LINEAR;
}


Tween::EasingKernel Tween::getKernel(TweenAlgorithm type) {
    switch (type) {
        case EASE_IN_QUADRATIC:      return &Tween::easeInQuadratic;
        case EASE_OUT_QUADRATIC:     return &Tween::easeOutQuadratic;
        case EASE_INOUT_QUADRATIC:   return &Tween::easeInOutQuadratic;
        case EASE_IN_CUBIC:          return &Tween::easeInCubic;
        case EASE_OUT_CUBIC:         return &Tween::easeOutCubic;
        case EASE_INOUT_CUBIC:       return &Tween::easeInOutCubic;
        case EASE_IN_QUARTIC:        return &Tween::easeInQuartic;
        case EASE_OUT_QUARTIC:       return &Tween::easeOutQuartic;
        case EASE_INOUT_QUARTIC:     return &Tween::easeInOutQuartic;
        case EASE_IN_QUINTIC:        return &Tween::easeInQuintic;
        case EASE_OUT_QUINTIC:       return &Tween::easeOutQuintic;
        case EASE_INOUT_QUINTIC:     return &Tween::easeInOutQuintic;
        case EASE_IN_SINE:           return &Tween::easeInSine;
        case EASE_OUT_SINE:          return &Tween::easeOutSine;
        case EASE_INOUT_SINE:        return &Tween::easeInOutSine;
        case EASE_IN_EXPONENTIAL:    return &Tween::easeInExponential;
        case EASE_OUT_EXPONENTIAL:   return &Tween::easeOutExponential;
        case EASE_INOUT_EXPONENTIAL: return &Tween::easeInOutExponential;
        case EASE_IN_CIRCULAR:       return &Tween::easeInCircular;
        case EASE_OUT_CIRCULAR:      return &Tween::easeOutCircular;
        case EASE_INOUT_CIRCULAR:    return &Tween::easeInOutCircular;
        case LINEAR:
        default:                     return &Tween::linear;
    }
}

void Tween::evaluateBatch(TweenAlgorithm type, const float* progress, const float* start, const float* change, float* out, size_t count) {
    switch (type) {
        case EASE_IN_QUADRATIC:
#if defined(__SSE2__)
            evaluateEaseInQuadraticSse(progress, start, change, out, count);
#else
            for (size_t i = 0; i < count; ++i) out[i] = easeInQuadratic(progress[i], start[i], change[i]);
#endif
            break;
        case EASE_OUT_QUADRATIC:
#if defined(__SSE2__)
            evaluateEaseOutQuadraticSse(progress, start, change, out, count);
#else
            for (size_t i = 0; i < count; ++i) out[i] = easeOutQuadratic(progress[i], start[i], change[i]);
#endif
            break;
        case EASE_INOUT_QUADRATIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeInOutQuadratic(progress[i], start[i], change[i]);
            break;
        case EASE_IN_CUBIC:
#if defined(__SSE2__)
            evaluateEaseInCubicSse(progress, start, change, out, count);
#else
            for (size_t i = 0; i < count; ++i) out[i] = easeInCubic(progress[i], start[i], change[i]);
#endif
            break;
        case EASE_OUT_CUBIC:
#if defined(__SSE2__)
            evaluateEaseOutCubicSse(progress, start, change, out, count);
#else
            for (size_t i = 0; i < count; ++i) out[i] = easeOutCubic(progress[i], start[i], change[i]);
#endif
            break;
        case EASE_INOUT_CUBIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeInOutCubic(progress[i], start[i], change[i]);
            break;
        case EASE_IN_QUARTIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeInQuartic(progress[i], start[i], change[i]);
            break;
        case EASE_OUT_QUARTIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeOutQuartic(progress[i], start[i], change[i]);
            break;
        case EASE_INOUT_QUARTIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeInOutQuartic(progress[i], start[i], change[i]);
            break;
        case EASE_IN_QUINTIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeInQuintic(progress[i], start[i], change[i]);
            break;
        case EASE_OUT_QUINTIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeOutQuintic(progress[i], start[i], change[i]);
            break;
        case EASE_INOUT_QUINTIC:
            for (size_t i = 0; i < count; ++i) out[i] = easeInOutQuintic(progress[i], start[i], change[i]);
            break;
        case EASE_IN_SINE:
            for (size_t i = 0; i < count; ++i) out[i] = easeInSine(progress[i], start[i], change[i]);
            break;
        case EASE_OUT_SINE:
            for (size_t i = 0; i < count; ++i) out[i] = easeOutSine(progress[i], start[i], change[i]);
            break;
        case EASE_INOUT_SINE:
            for (size_t i = 0; i < count; ++i) out[i] = easeInOutSine(progress[i], start[i], change[i]);
            break;
        case EASE_IN_EXPONENTIAL:
            for (size_t i = 0; i < count; ++i) out[i] = easeInExponential(progress[i], start[i], change[i]);
            break;
        case EASE_OUT_EXPONENTIAL:
            for (size_t i = 0; i < count; ++i) out[i] = easeOutExponential(progress[i], start[i], change[i]);
            break;
        case EASE_INOUT_EXPONENTIAL:
            for (size_t i = 0; i < count; ++i) out[i] = easeInOutExponential(progress[i], start[i], change[i]);
            break;
        case EASE_IN_CIRCULAR:
            for (size_t i = 0; i < count; ++i) out[i] = easeInCircular(progress[i], start[i], change[i]);
            break;
        case EASE_OUT_CIRCULAR:
            for (size_t i = 0; i < count; ++i) out[i] = easeOutCircular(progress[i], start[i], change[i]);
            break;
        case EASE_INOUT_CIRCULAR:
            for (size_t i = 0; i < count; ++i) out[i] = easeInOutCircular(progress[i], start[i], change[i]);
            break;
        case LINEAR:
        default:
#if defined(__SSE2__)
            evaluateLinearSse(progress, start, change, out, count);
#else
            for (size_t i = 0; i < count; ++i) out[i] = linear(progress[i], start[i], change[i]);
#endif
            break;
    }
}

float Tween::animate(double elapsedTime) const {
    return animateSingle(type, start, end, duration, static_cast<float>(elapsedTime));
}

float Tween::animate(double elapsedTime, float startValue) const {
    return animateSingle(type, startValue, end, duration, static_cast<float>(elapsedTime));
}

float Tween::animateSingle(TweenAlgorithm type, float start, float end, float duration, float elapsedTime) {
    // If duration is zero or negative, animation is instant. Return the end state.
    if (duration <= 0.0f) {
        return end;
    }

    // Clamp time to prevent overshooting the animation.
    elapsedTime = std::min(elapsedTime, duration);

    // OPTIMIZATION: Calculate normalized progress (0.0 to 1.0) once.
    const float progress = elapsedTime / duration;
    const float change = end - start;

    switch (type) {
        case EASE_IN_QUADRATIC:     return easeInQuadratic(progress, start, change);
        case EASE_OUT_QUADRATIC:    return easeOutQuadratic(progress, start, change);
        case EASE_INOUT_QUADRATIC:  return easeInOutQuadratic(progress, start, change);
        case EASE_IN_CUBIC:         return easeInCubic(progress, start, change);
        case EASE_OUT_CUBIC:        return easeOutCubic(progress, start, change);
        case EASE_INOUT_CUBIC:      return easeInOutCubic(progress, start, change);
        case EASE_IN_QUARTIC:       return easeInQuartic(progress, start, change);
        case EASE_OUT_QUARTIC:      return easeOutQuartic(progress, start, change);
        case EASE_INOUT_QUARTIC:    return easeInOutQuartic(progress, start, change);
        case EASE_IN_QUINTIC:       return easeInQuintic(progress, start, change);
        case EASE_OUT_QUINTIC:      return easeOutQuintic(progress, start, change);
        case EASE_INOUT_QUINTIC:    return easeInOutQuintic(progress, start, change);
        case EASE_IN_SINE:          return easeInSine(progress, start, change);
        case EASE_OUT_SINE:         return easeOutSine(progress, start, change);
        case EASE_INOUT_SINE:       return easeInOutSine(progress, start, change);
        case EASE_IN_EXPONENTIAL:   return easeInExponential(progress, start, change);
        case EASE_OUT_EXPONENTIAL:  return easeOutExponential(progress, start, change);
        case EASE_INOUT_EXPONENTIAL:return easeInOutExponential(progress, start, change);
        case EASE_IN_CIRCULAR:      return easeInCircular(progress, start, change);
        case EASE_OUT_CIRCULAR:     return easeOutCircular(progress, start, change);
        case EASE_INOUT_CIRCULAR:   return easeInOutCircular(progress, start, change);
        case LINEAR:
        default:                    return linear(progress, start, change);
    }
}

// NOTE: All easing functions now use the new signature:
// (float progress, float start_value, float change_in_value)
// 'p' is progress (0.0 to 1.0), 'b' is the beginning value, 'c' is the total change.

float Tween::linear(float p, float b, float c) {
    return c * p + b;
}

float Tween::easeInQuadratic(float p, float b, float c) {
    return c * p * p + b;
}

float Tween::easeOutQuadratic(float p, float b, float c) {
    return -c * p * (p - 2.0f) + b;
}

float Tween::easeInOutQuadratic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p + b;
    p--;
    return -c / 2.0f * (p * (p - 2.0f) - 1.0f) + b;
}

float Tween::easeInCubic(float p, float b, float c) {
    return c * p * p * p + b;
}

float Tween::easeOutCubic(float p, float b, float c) {
    p--;
    return c * (p * p * p + 1.0f) + b;
}

float Tween::easeInOutCubic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p * p + b;
    p -= 2.0f;
    return c / 2.0f * (p * p * p + 2.0f) + b;
}

float Tween::easeInQuartic(float p, float b, float c) {
    return c * p * p * p * p + b;
}

float Tween::easeOutQuartic(float p, float b, float c) {
    p--;
    return -c * (p * p * p * p - 1.0f) + b;
}

float Tween::easeInOutQuartic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p * p * p + b;
    p -= 2.0f;
    return -c / 2.0f * (p * p * p * p - 2.0f) + b;
}

float Tween::easeInQuintic(float p, float b, float c) {
    return c * p * p * p * p * p + b;
}

float Tween::easeOutQuintic(float p, float b, float c) {
    p--;
    return c * (p * p * p * p * p + 1.0f) + b;
}

float Tween::easeInOutQuintic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p * p * p * p + b;
    p -= 2.0f;
    return c / 2.0f * (p * p * p * p * p + 2.0f) + b;
}

float Tween::easeInSine(float p, float b, float c) {
    return -c * cosf(p * ((float)M_PI / 2.0f)) + c + b;
}

float Tween::easeOutSine(float p, float b, float c) {
    return c * sinf(p * ((float)M_PI / 2.0f)) + b;
}

float Tween::easeInOutSine(float p, float b, float c) {
    return -c / 2.0f * (cosf((float)M_PI * p) - 1.0f) + b;
}

float Tween::easeInExponential(float p, float b, float c) {
    return c * powf(2.0f, 10.0f * (p - 1.0f)) + b;
}

float Tween::easeOutExponential(float p, float b, float c) {
    return c * (-powf(2.0f, -10.0f * p) + 1.0f) + b;
}

float Tween::easeInOutExponential(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * powf(2.0f, 10.0f * (p - 1.0f)) + b;
    p--;
    return c / 2.0f * (-powf(2.0f, -10.0f * p) + 2.0f) + b;
}

float Tween::easeInCircular(float p, float b, float c) {
    return -c * (sqrtf(1.0f - p * p) - 1.0f) + b;
}

float Tween::easeOutCircular(float p, float b, float c) {
    p--;
    return c * sqrtf(1.0f - p * p) + b;
}

float Tween::easeInOutCircular(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return -c / 2.0f * (sqrtf(1.0f - p * p) - 1.0f) + b;
    p -= 2.0f;
    return c / 2.0f * (sqrtf(1.0f - p * p) + 1.0f);
}
