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
    return CompiledTweenEntry {
        tween.property,
        tween.algorithm(),
        tween.duration,
        tween.startDefined,
        tween.startValue(),
        tween.endValue(),
        tween.playlistTokenIds()
    };
}

TweenSet::TweenSet(const TweenSet& copy) {
    set_.reserve(copy.set_.size());
    compiledSet_.reserve(copy.compiledSet_.size());
    for (const auto& tween : copy.set_) {
        set_.push_back(std::make_unique<Tween>(*tween));
    }
    compiledSet_ = copy.compiledSet_;
}

TweenSet& TweenSet::operator=(const TweenSet& other) {
    if (this != &other) {
        set_.clear(); // Clear existing tweens
        compiledSet_.clear();
        set_.reserve(other.set_.size());
        compiledSet_.reserve(other.compiledSet_.size());
        for (const auto& tween : other.set_) {
            set_.push_back(std::make_unique<Tween>(*tween));
        }
        compiledSet_ = other.compiledSet_;
    }
    return *this;
}


TweenSet::~TweenSet()
{
    clear();
}

void TweenSet::push(std::unique_ptr<Tween> tween) {
    if (tween) {
        compiledSet_.push_back(compileTween(*tween));
    }
    set_.push_back(std::move(tween));
}

void TweenSet::clear() {
    set_.clear();
    compiledSet_.clear();
}

Tween* TweenSet::getTween(unsigned int index) const {
    if (index < set_.size()) {
        return set_[index].get();
    }
    return nullptr;
}

const std::vector<TweenSet::CompiledTweenEntry>& TweenSet::compiledTweens() const {
    return compiledSet_;
}

size_t TweenSet::size() const
{
    return compiledSet_.size();
}
