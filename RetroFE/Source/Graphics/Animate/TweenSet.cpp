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
#include "TweenSet.h"

TweenSet::TweenSet() = default;

TweenSet::CompiledTweenEntry TweenSet::compileTween(const Tween& tween) {
    const float duration = tween.duration;
    const float deltaValue = tween.endValue() - tween.startValue();
    return CompiledTweenEntry {
        tween.property,
        tween.algorithm(),
        duration,
        duration > 0.0f ? (1.0f / duration) : 0.0f,
        tween.startDefined,
        tween.startValue(),
        tween.endValue(),
        deltaValue,
        tween.playlistTokenIds()
    };
}

TweenSet::TweenSet(const TweenSet& copy)
    : compiledSet_(copy.compiledSet_),
      compiledByAlgorithm_(copy.compiledByAlgorithm_) {
}

TweenSet& TweenSet::operator=(const TweenSet& other) {
    if (this != &other) {
        compiledSet_ = other.compiledSet_;
        compiledByAlgorithm_ = other.compiledByAlgorithm_;
    }
    return *this;
}

TweenSet::~TweenSet()
{
    clear();
}

void TweenSet::push(std::unique_ptr<Tween> tween) {
    if (tween) {
        pushCompiled(compileTween(*tween));
    }
}


void TweenSet::pushCompiled(CompiledTweenEntry tween) {
    tween.deltaValue = tween.endValue - tween.startValue;
    tween.invDuration = tween.duration > 0.0f ? (1.0f / tween.duration) : 0.0f;

    const size_t algorithmIndex = static_cast<size_t>(tween.algorithm);
    if (algorithmIndex < kTweenAlgorithmCount) {
        compiledByAlgorithm_[algorithmIndex].push_back(compiledSet_.size());
    }

    compiledSet_.push_back(std::move(tween));
}

void TweenSet::clear() {
    compiledSet_.clear();
    for (auto& bucket : compiledByAlgorithm_) {
        bucket.clear();
    }
}

const std::vector<TweenSet::CompiledTweenEntry>& TweenSet::compiledTweens() const {
    return compiledSet_;
}

const std::array<std::vector<size_t>, TweenSet::kTweenAlgorithmCount>& TweenSet::compiledTweensByAlgorithm() const {
    return compiledByAlgorithm_;
}

size_t TweenSet::size() const
{
    return compiledSet_.size();
}
