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
#include "Animation.h"
#include <utility> // Required for std::move

 // NOTE: The copy-assignment operator was removed to support move-only containers.

void Animation::Push(TweenSet&& set) {
    // Correctly move the move-only TweenSet into the vector.
    // This avoids a deep copy of all internal tweens and strings.
    animationVector_.push_back(std::move(set));
}

void Animation::Clear() {
    // Physically releases the contiguous memory of the vector.
    animationVector_.clear();
}

TweenSet& Animation::operator[](size_t index) {
    // Direct index access for maximum performance in the render loop.
    // Using .at() provides bounds checking safety.
    return animationVector_.at(index);
}

const TweenSet& Animation::operator[](size_t index) const {
    return animationVector_.at(index);
}

size_t Animation::size() const {
    return animationVector_.size();
}