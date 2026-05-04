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

class TweenSet {
public:
    TweenSet() = default;

    // Support moving the entire set (efficiently transfers the vector)
    TweenSet(TweenSet&&) noexcept = default;
    TweenSet& operator=(TweenSet&&) noexcept = default;

    // Strictly forbid copying to prevent memory churn
    TweenSet(const TweenSet&) = delete;
    TweenSet& operator=(const TweenSet&) = delete;

    void push(Tween&& tween);
    void clear();

    Tween& operator[](size_t index);
    const Tween& operator[](size_t index) const;

    size_t size() const;

private:
    std::vector<Tween> set_;
};