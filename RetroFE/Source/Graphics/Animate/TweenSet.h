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
#include <memory>

class TweenSet {
public:
    TweenSet();
    // Default copy/assignment now work correctly with std::vector<Tween>
    TweenSet(const TweenSet& copy) = default;
    TweenSet& operator=(const TweenSet& other) = default;
    ~TweenSet() = default;

    void push(const Tween& tween); // Changed to take object by reference
    void clear();
    void clearDynamicTweens();
    Tween* getTween(unsigned int index); // Returns pointer to internal object

    size_t size() const;

private:
    std::vector<Tween> set_; // Contiguous storage of Tween objects
};