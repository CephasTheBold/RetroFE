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
#include <string>
#include <vector>
#include <map>
#include <memory>

class Animation {
public:
    Animation();
    Animation(const Animation& copy) = default;
    Animation& operator=(const Animation& other) = default;
    ~Animation() = default;

    void Push(const TweenSet& set); // Takes a copy of the set
    void Clear();

    TweenSet* tweenSet(unsigned int index); // Returns pointer to internal set
    size_t size() const;

private:
    std::vector<TweenSet> animationVector_; // Contiguous storage of sets
};
