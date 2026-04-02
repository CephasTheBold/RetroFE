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
#include <string>
#include <memory>
#include <map>

Animation* AnimationEvents::getAnimation(const std::string& tween, int index) {
    // Ensure the tween group exists
    if (animationMap_.find(tween) == animationMap_.end()) {
        animationMap_[tween][-1] = Animation();
    }

    auto& group = animationMap_[tween];
    auto it = group.find(index);
    if (it == group.end()) {
        index = -1; // Fallback to default index
    }

    return &group[index];
}

const std::map<std::string, std::map<int, Animation>>& AnimationEvents::getAnimationMap() const {
    return animationMap_;
}

void AnimationEvents::setAnimation(const std::string& tween, int index, const Animation& animation) {
    animationMap_[tween][index] = animation; // Performs a deep copy
}

void AnimationEvents::clear() {
    animationMap_.clear();
}