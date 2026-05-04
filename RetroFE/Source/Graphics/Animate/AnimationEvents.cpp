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

std::shared_ptr<Animation> AnimationEvents::getAnimation(const std::string& tween, int index) {
    auto& group = animationMap_[tween];
    auto it = group.find(index);

    if (it == group.end()) {
        it = group.find(-1);
        if (it == group.end()) return nullptr;
    }

    // FIX: Return the shared_ptr directly (remove .get())
    return it->second;
}

void AnimationEvents::setAnimation(const std::string& tween, int index, std::shared_ptr<Animation> animation) {
    // Zero-Churn: Multiple keys now point to the same physical memory
    animationMap_[tween][index] = std::move(animation);
}

void AnimationEvents::clear() {
    animationMap_.clear();
}