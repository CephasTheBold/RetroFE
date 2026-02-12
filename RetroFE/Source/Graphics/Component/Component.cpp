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
#include "Component.h"
#include "../Animate/Tween.h"
#include "../../Graphics/ViewInfo.h"
#include "../../Utility/Log.h"
#include "../../SDL.h"
#include "../PageBuilder.h"
#include <vector>

Component::Component(Page &p)
: page(p)
{
    tweens_                   = nullptr;
    menuScrollReload_         = false;
    animationDoneRemove_      = false;
    id_                       = -1;
    backgroundTexture_ = nullptr;
    animationRequestedType_ = "";
    animationType_ = "";
    animationRequested_ = false;
    newItemSelected = false;
    newScrollItemSelected = false;
    menuIndex_ = -1;

    currentTweenIndex_ = 0;
    currentTweenComplete_ = true;
    elapsedTweenTime_ = 0;


}

Component::~Component() = default;

void Component::freeGraphicsMemory() {
    animationRequestedType_ = "";
    animationType_ = "";
    animationRequested_ = false;
    newItemSelected = false;
    newScrollItemSelected = false;
    menuIndex_ = -1;

    // Reset the shared_ptr to release the memory
    currentTweens_.reset();
    currentTweenIndex_ = 0;
    currentTweenComplete_ = true;
    elapsedTweenTime_ = 0;

    tweenEvaluations_.clear();
    for (auto& bucket : tweenAlgorithmBuckets_) {
        bucket.clear();
    }
    tweenProgressBatch_.clear();
    tweenStartBatch_.clear();
    tweenChangeBatch_.clear();
    tweenValueBatch_.clear();
    tweenOutputIndices_.clear();

    if (backgroundTexture_) {
        SDL_DestroyTexture(backgroundTexture_);
        backgroundTexture_ = nullptr;
    }
}

// used to draw lines in the layout using <container>
void Component::allocateGraphicsMemory()
{
    if (!backgroundTexture_) {
        // make a 4x4 pixel wide surface to be stretched during rendering, make it a white background so we can use
        // color  later
        SDL_Surface* surface = SDL_CreateRGBSurface(0, 4, 4, 32, 0, 0, 0, 0);
        SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, 255, 255, 255));

        backgroundTexture_ = SDL_CreateTextureFromSurface(SDL::getRenderer(baseViewInfo.Monitor), surface);

        SDL_FreeSurface(surface);
        SDL_SetTextureBlendMode(backgroundTexture_, SDL_BLENDMODE_BLEND);
    }
}


void Component::deInitializeFonts()
{
}


void Component::initializeFonts()
{
}

const std::string& Component::getAnimationRequestedType() const {
    return animationRequestedType_;
}

void Component::triggerEvent(const std::string_view& event, int menuIndex)
{
    animationRequestedType_ = event;
    animationRequested_     = true;
    menuIndex_              = (menuIndex > 0 ? menuIndex : 0);
}

void Component::setPlaylist(const std::string_view& name)
{
    this->playlistName = name;
}

void Component::setNewItemSelected()
{
    newItemSelected = true;
}

void Component::setNewScrollItemSelected()
{
    newScrollItemSelected = true;
}

void Component::setId( int id )
{
    id_ = id;
}

bool Component::isIdle() const
{
    return (currentTweenComplete_ || animationType_ == "idle" || animationType_ == "menuIdle" || animationType_ == "attract");
}

bool Component::isAttractIdle() const
{
    return (currentTweenComplete_ || animationType_ == "idle" || animationType_ == "menuIdle");
}

bool Component::isMenuScrolling() const
{
    return (!currentTweenComplete_ && (animationType_ == "menuScroll" || animationType_ == "playlistScroll"));
}

bool Component::isPlaylistScrolling() const
{
    return (!currentTweenComplete_ && animationType_ == "playlistScroll");
}

void Component::setTweens(std::shared_ptr<AnimationEvents> set) {
    tweens_ = std::move(set);
}

std::string_view Component::filePath()
{
    return "";
}

bool Component::update(float dt) {
    elapsedTweenTime_ += dt;
    if (animationRequested_ && animationRequestedType_ != "" && !tweens_->getAnimationMap().empty()) {
        std::shared_ptr<Animation> newTweens = nullptr;
        if (menuIndex_ >= MENU_INDEX_HIGH) {
            newTweens = tweens_->getAnimation(animationRequestedType_, MENU_INDEX_HIGH);
            if (!(newTweens && newTweens->size() > 0)) {
                newTweens = tweens_->getAnimation(animationRequestedType_, menuIndex_ - MENU_INDEX_HIGH);
            }
        }
        else {
            newTweens = tweens_->getAnimation(animationRequestedType_, menuIndex_);
        }

        if (newTweens && newTweens->size() == 0) {
            newTweens.reset();
        }

        if (newTweens && newTweens->size() > 0) {
            animationType_ = animationRequestedType_;
            currentTweens_ = newTweens;  // Assign to weak_ptr
            currentTweenIndex_ = 0;
            elapsedTweenTime_ = 0;
            storeViewInfo_ = baseViewInfo;
            currentTweenComplete_ = false;
        }
        animationRequested_ = false;
    }

    if (tweens_ && currentTweenComplete_) {
        animationType_ = "idle";
        auto idleTweens = tweens_->getAnimation("idle", menuIndex_);
        if (idleTweens && idleTweens->size() == 0 && !page.isMenuScrolling()) {
            idleTweens = tweens_->getAnimation("menuIdle", menuIndex_);
        }
        currentTweens_ = idleTweens;  // Assign to weak_ptr
        currentTweenIndex_ = 0;
        elapsedTweenTime_ = 0;
        storeViewInfo_ = baseViewInfo;
        currentTweenComplete_ = false;
        animationRequested_ = false;
    }

    // Lock weak_ptr before using
    std::shared_ptr<Animation> lockedTweens = currentTweens_.lock();
    if (lockedTweens) {
        currentTweenComplete_ = animate();
        if (currentTweenComplete_) {
            currentTweens_.reset();
            currentTweenIndex_ = 0;
        }
    }
    else {
        currentTweenComplete_ = true;
    }

    return currentTweenComplete_;
}

// used to draw lines in the layout using <container>
void Component::draw()
{
    if (backgroundTexture_ && baseViewInfo.Alpha > 0.0f) {
        SDL_FRect rect = { 0,0,0,0 };
        rect.h = baseViewInfo.ScaledHeight();
        rect.w = baseViewInfo.ScaledWidth();
        rect.x = baseViewInfo.XRelativeToOrigin();
        rect.y = baseViewInfo.YRelativeToOrigin();


        SDL_SetTextureColorMod(backgroundTexture_,
            static_cast<char>(baseViewInfo.BackgroundRed * 255),
            static_cast<char>(baseViewInfo.BackgroundGreen * 255),
            static_cast<char>(baseViewInfo.BackgroundBlue * 255));

        SDL::renderCopyF(backgroundTexture_, baseViewInfo.BackgroundAlpha, nullptr, &rect, baseViewInfo, page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
    }
}

bool Component::animate() {
    bool completeDone = false;
    auto sharedTweens = currentTweens_.lock(); // Lock the weak_ptr to get a shared_ptr

    if (!sharedTweens || currentTweenIndex_ >= sharedTweens->size()) {
        completeDone = true;
    }
    else {
        bool currentDone = true;
        auto tweens = sharedTweens->tweenSet(currentTweenIndex_);
        if (!tweens) return true; // Additional check for safety

        tweenEvaluations_.clear();
        tweenEvaluations_.reserve(tweens->size());
        for (auto& bucket : tweenAlgorithmBuckets_) {
            bucket.clear();
        }

        auto getStoreValueForProperty = [this](TweenProperty property) -> float {
            switch (property) {
                case TWEEN_PROPERTY_X:                return storeViewInfo_.X;
                case TWEEN_PROPERTY_Y:                return storeViewInfo_.Y;
                case TWEEN_PROPERTY_HEIGHT:           return storeViewInfo_.Height;
                case TWEEN_PROPERTY_WIDTH:            return storeViewInfo_.Width;
                case TWEEN_PROPERTY_ANGLE:            return storeViewInfo_.Angle;
                case TWEEN_PROPERTY_ALPHA:            return storeViewInfo_.Alpha;
                case TWEEN_PROPERTY_X_ORIGIN:         return storeViewInfo_.XOrigin;
                case TWEEN_PROPERTY_Y_ORIGIN:         return storeViewInfo_.YOrigin;
                case TWEEN_PROPERTY_X_OFFSET:         return storeViewInfo_.XOffset;
                case TWEEN_PROPERTY_Y_OFFSET:         return storeViewInfo_.YOffset;
                case TWEEN_PROPERTY_FONT_SIZE:        return storeViewInfo_.FontSize;
                case TWEEN_PROPERTY_BACKGROUND_ALPHA: return storeViewInfo_.BackgroundAlpha;
                case TWEEN_PROPERTY_MAX_WIDTH:        return storeViewInfo_.MaxWidth;
                case TWEEN_PROPERTY_MAX_HEIGHT:       return storeViewInfo_.MaxHeight;
                case TWEEN_PROPERTY_LAYER:            return static_cast<float>(storeViewInfo_.Layer);
                case TWEEN_PROPERTY_CONTAINER_X:      return storeViewInfo_.ContainerX;
                case TWEEN_PROPERTY_CONTAINER_Y:      return storeViewInfo_.ContainerY;
                case TWEEN_PROPERTY_CONTAINER_WIDTH:  return storeViewInfo_.ContainerWidth;
                case TWEEN_PROPERTY_CONTAINER_HEIGHT: return storeViewInfo_.ContainerHeight;
                case TWEEN_PROPERTY_VOLUME:           return storeViewInfo_.Volume;
                case TWEEN_PROPERTY_MONITOR:          return static_cast<float>(storeViewInfo_.Monitor);
                case TWEEN_PROPERTY_NOP:
                case TWEEN_PROPERTY_RESTART:
                default:                              return 0.0f;
            }
        };

        for (unsigned int i = 0; i < tweens->size(); i++) {
            const Tween* tween = tweens->getTween(i); // Ensure const correctness
            if (!tween || !tween->matchesPlaylist(playlistName)) {
                continue;
            }

            double elapsedTime = elapsedTweenTime_;
            if (elapsedTime < tween->duration) {
                currentDone = false;
            }
            else {
                elapsedTime = tween->duration;
            }

            const auto algorithmIndex = static_cast<size_t>(tween->algorithm());
            if (algorithmIndex >= kTweenAlgorithmCount) {
                continue;
            }

            const float resolvedStartValue = tween->startDefined
                ? tween->startValue()
                : getStoreValueForProperty(tween->property);

            TweenEvaluation evaluation {
                tween,
                tween->property,
                elapsedTime,
                resolvedStartValue,
                0.0f
            };

            const size_t evaluationIndex = tweenEvaluations_.size();
            tweenEvaluations_.push_back(evaluation);
            tweenAlgorithmBuckets_[algorithmIndex].push_back(evaluationIndex);
        }

        for (size_t algorithmIndex = 0; algorithmIndex < kTweenAlgorithmCount; ++algorithmIndex) {
            const auto algorithm = static_cast<TweenAlgorithm>(algorithmIndex);
            const auto& bucket = tweenAlgorithmBuckets_[algorithmIndex];
            if (bucket.empty()) {
                continue;
            }

            tweenProgressBatch_.clear();
            tweenStartBatch_.clear();
            tweenChangeBatch_.clear();
            tweenValueBatch_.clear();
            tweenOutputIndices_.clear();
            tweenProgressBatch_.reserve(bucket.size());
            tweenStartBatch_.reserve(bucket.size());
            tweenChangeBatch_.reserve(bucket.size());
            tweenValueBatch_.reserve(bucket.size());
            tweenOutputIndices_.reserve(bucket.size());

            for (const size_t evaluationIndex : bucket) {
                auto& evaluation = tweenEvaluations_[evaluationIndex];
                const float endValue = evaluation.tween->endValue();
                const float duration = evaluation.tween->duration;

                if (duration <= 0.0f) {
                    evaluation.value = endValue;
                    continue;
                }

                tweenProgressBatch_.push_back(static_cast<float>(evaluation.elapsedTime) / duration);
                tweenStartBatch_.push_back(evaluation.startValue);
                tweenChangeBatch_.push_back(endValue - evaluation.startValue);
                tweenOutputIndices_.push_back(evaluationIndex);
            }

            if (tweenOutputIndices_.empty()) {
                continue;
            }

            tweenValueBatch_.resize(tweenOutputIndices_.size());
            Tween::evaluateBatch(algorithm,
                tweenProgressBatch_.data(),
                tweenStartBatch_.data(),
                tweenChangeBatch_.data(),
                tweenValueBatch_.data(),
                tweenValueBatch_.size());

            for (size_t i = 0; i < tweenOutputIndices_.size(); ++i) {
                tweenEvaluations_[tweenOutputIndices_[i]].value = tweenValueBatch_[i];
            }
        }

        for (const auto& evaluation : tweenEvaluations_) {
            const float value = evaluation.value;
            switch (evaluation.property) {
                case TWEEN_PROPERTY_X:
                    baseViewInfo.X = value;
                    break;

                case TWEEN_PROPERTY_Y:
                    baseViewInfo.Y = value;
                    break;

                case TWEEN_PROPERTY_HEIGHT:
                    baseViewInfo.Height = value;
                    break;

                case TWEEN_PROPERTY_WIDTH:
                    baseViewInfo.Width = value;
                    break;

                case TWEEN_PROPERTY_ANGLE:
                    baseViewInfo.Angle = value;
                    break;

                case TWEEN_PROPERTY_ALPHA:
                    baseViewInfo.Alpha = value;
                    break;

                case TWEEN_PROPERTY_X_ORIGIN:
                    baseViewInfo.XOrigin = value;
                    break;

                case TWEEN_PROPERTY_Y_ORIGIN:
                    baseViewInfo.YOrigin = value;
                    break;

                case TWEEN_PROPERTY_X_OFFSET:
                    baseViewInfo.XOffset = value;
                    break;

                case TWEEN_PROPERTY_Y_OFFSET:
                    baseViewInfo.YOffset = value;
                    break;

                case TWEEN_PROPERTY_FONT_SIZE:
                    baseViewInfo.FontSize = value;
                    break;

                case TWEEN_PROPERTY_BACKGROUND_ALPHA:
                    baseViewInfo.BackgroundAlpha = value;
                    break;

                case TWEEN_PROPERTY_MAX_WIDTH:
                    baseViewInfo.MaxWidth = value;
                    break;

                case TWEEN_PROPERTY_MAX_HEIGHT:
                    baseViewInfo.MaxHeight = value;
                    break;

                case TWEEN_PROPERTY_LAYER:
                    baseViewInfo.Layer = static_cast<unsigned int>(value);
                    break;

                case TWEEN_PROPERTY_CONTAINER_X:
                    baseViewInfo.ContainerX = value;
                    break;

                case TWEEN_PROPERTY_CONTAINER_Y:
                    baseViewInfo.ContainerY = value;
                    break;

                case TWEEN_PROPERTY_CONTAINER_WIDTH:
                    baseViewInfo.ContainerWidth = value;
                    break;

                case TWEEN_PROPERTY_CONTAINER_HEIGHT:
                    baseViewInfo.ContainerHeight = value;
                    break;

                case TWEEN_PROPERTY_VOLUME:
                    baseViewInfo.Volume = value;
                    break;

                case TWEEN_PROPERTY_MONITOR:
                    baseViewInfo.Monitor = static_cast<unsigned int>(value);
                    break;

                case TWEEN_PROPERTY_NOP:
                    break;

                case TWEEN_PROPERTY_RESTART:
                    baseViewInfo.Restart = (evaluation.tween->duration != 0.0f) && (evaluation.elapsedTime == 0.0);
                    break;
            }
        }

        if (currentDone) {
            currentTweenIndex_++;
            elapsedTweenTime_ = 0;
            storeViewInfo_ = baseViewInfo;
        }
    }

    if (!sharedTweens || currentTweenIndex_ >= sharedTweens->size()) {
        completeDone = true;
    }

    return completeDone;
}

bool Component::isPlaying()
{
    return false;
}



bool Component::isJukeboxPlaying()
{
    return false;
}


void Component::setMenuScrollReload(bool menuScrollReload)
{
    menuScrollReload_ = menuScrollReload;
}


bool Component::getMenuScrollReload() const
{
    return menuScrollReload_;
}

void Component::setAnimationDoneRemove(bool value)
{
    animationDoneRemove_ = value;
}

bool Component::getAnimationDoneRemove() const
{
    return animationDoneRemove_;
}

int Component::getId( ) const
{
    return id_;
}

void Component::setPauseOnScroll(bool value)
{   
    pauseOnScroll_ = value;
}

bool Component::getPauseOnScroll() const 
{
    return pauseOnScroll_;
}