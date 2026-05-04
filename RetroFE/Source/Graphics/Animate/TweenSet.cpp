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
#include <stdexcept>

void TweenSet::push(Tween&& tween) {
    // Correctly move the move-only Tween into the vector
    set_.push_back(std::move(tween));
}

void TweenSet::clear() {
    set_.clear();
}

Tween& TweenSet::operator[](size_t index) {
    // Standard vector access is O(1)
    return set_.at(index);
}

const Tween& TweenSet::operator[](size_t index) const {
    return set_.at(index);
}

size_t TweenSet::size() const {
    return set_.size();
}