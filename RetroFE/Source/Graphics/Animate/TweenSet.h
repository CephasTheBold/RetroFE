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

#include "Tween.h"
#include <vector>
#include <memory>
#include <cstdint>
#include <array>

class TweenSet
{
public:
    static constexpr size_t kTweenAlgorithmCount = static_cast<size_t>(EASE_INOUT_CIRCULAR) + 1;

    struct CompiledTweenEntry {
        TweenProperty property;
        TweenAlgorithm algorithm;
        float duration;
        float invDuration;
        bool startDefined;
        float startValue;
        float endValue;
        float deltaValue;
        std::vector<uint32_t> playlistTokenIds;
    };

    TweenSet();
    TweenSet(const TweenSet& copy);
    TweenSet& operator=(const TweenSet& other);
    ~TweenSet();
    void push(std::unique_ptr<Tween> tween);
    void pushCompiled(CompiledTweenEntry tween);
    void clear();
    const std::vector<CompiledTweenEntry>& compiledTweens() const;
    const std::array<std::vector<size_t>, kTweenAlgorithmCount>& compiledTweensByAlgorithm() const;

    size_t size() const;

private:
    static CompiledTweenEntry compileTween(const Tween& tween);

    std::vector<CompiledTweenEntry> compiledSet_;
    std::array<std::vector<size_t>, kTweenAlgorithmCount> compiledByAlgorithm_;
};
