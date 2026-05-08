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
#include "AnimationEvents.h"

Animation* AnimationEvents::getAnimation(std::string_view tween, int index) {
    // C++20: find() now accepts string_view without allocation
    auto itGroup = animationMap_.find(tween);

    // Maintain original behavior: Create the group if it doesn't exist.
    // This is required for the "menuScroll" scratchpad in ScrollingList.
    if (itGroup == animationMap_.end()) {
        // We convert to std::string once here during creation
        animationMap_[std::string(tween)][-1] = Animation();
        itGroup = animationMap_.find(tween);
    }

    auto& group = itGroup->second;
    auto it = group.find(index);
    if (it == group.end()) {
        index = -1; // Fallback to default index
    }

    // Still returns a pointer to the internal object (Get or Create behavior)
    return &group[index];
}

const AnimationEvents::AnimationMap& AnimationEvents::getAnimationMap() const {
    return animationMap_;
}

void AnimationEvents::setAnimation(const std::string& tween, int index, const Animation& animation) {
    animationMap_[tween][index] = animation;
}

void AnimationEvents::clear() {
    animationMap_.clear();
}