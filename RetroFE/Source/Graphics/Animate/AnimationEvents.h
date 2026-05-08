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
#include <string_view>

class AnimationEvents {
public:
    // C++20: Use std::less<> to enable transparent lookups
    using InnerMap = std::map<int, Animation>;
    using AnimationMap = std::map<std::string, InnerMap, std::less<>>;

    AnimationEvents() = default;
    ~AnimationEvents() = default;

    // C++20: Use std::string_view to avoid allocations during lookups
    Animation* getAnimation(std::string_view tween, int index = -1);

    void setAnimation(const std::string& tween, int index, const Animation& animation);
    void clear();

    const AnimationMap& getAnimationMap() const;

private:
    AnimationMap animationMap_;
};
