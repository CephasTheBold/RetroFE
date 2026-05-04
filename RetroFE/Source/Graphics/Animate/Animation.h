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
#include "TweenSet.h"
#include <vector>

class Animation {
public:
    Animation() = default;

    // Move-only semantics
    Animation(Animation&&) noexcept = default;
    Animation& operator=(Animation&&) noexcept = default;
    Animation(const Animation&) = delete;
    Animation& operator=(const Animation&) = delete;

    // Change to accept an rvalue to support moving
    void Push(TweenSet&& set);
    void Clear();

    TweenSet& operator[](size_t index);
    const TweenSet& operator[](size_t index) const;
    size_t size() const;

private:
    std::vector<TweenSet> animationVector_;
};