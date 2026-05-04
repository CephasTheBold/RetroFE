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
#include "Animation.h"
#include <map>
#include <string>
#include <memory>

class AnimationEvents {
public:
    AnimationEvents() = default;
    ~AnimationEvents() = default;

    // Still returns a raw pointer for Component to use safely
    std::shared_ptr<Animation> getAnimation(const std::string& tween, int index = -1);

    // Now accepts a shared_ptr to allow multiple indices to point to the same data
    void setAnimation(const std::string& tween, int index, std::shared_ptr<Animation> animation);
    void clear();

private:
    // OPTIMIZATION: Use shared_ptr to allow data sharing across indices
    std::map<std::string, std::map<int, std::shared_ptr<Animation>>> animationMap_;
};
