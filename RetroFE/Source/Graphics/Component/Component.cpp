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

Component::Component(Page &p)
: page(p)
{
    tweens_                   = nullptr;
    menuScrollReload_         = false;
    animationDoneRemove_      = false;
    id_                       = -1;
    backgroundTexture_ = nullptr;
    animationType_ = "";
    newRequests_.clear();
    newItemSelected = false;
    newScrollItemSelected = false;
    menuIndex_ = -1;
    currentAnimation_ = nullptr;
    currentTweenIndex_ = 0;
    currentTweenComplete_ = true;
    elapsedTweenTime_ = 0;


}

Component::~Component() = default;

void Component::freeGraphicsMemory() {
    animationType_ = "";
	newRequests_.clear();
    newItemSelected = false;
    newScrollItemSelected = false;
    menuIndex_ = -1;

    // Clear the locally owned animation data to release contiguous vector memory
    currentAnimation_ = nullptr;

    currentTweenIndex_ = 0;
    currentTweenComplete_ = true;
    elapsedTweenTime_ = 0;

    backgroundTexture_ = nullptr;
}

// used to draw lines in the layout using <container>
void Component::allocateGraphicsMemory() {
    int monitor = baseViewInfo.Monitor;
    if (sharedBackgroundTextures_.find(monitor) == sharedBackgroundTextures_.end()) {
        SDL_Surface* surface = SDL_CreateRGBSurface(0, 4, 4, 32, 0, 0, 0, 0);
        if (surface) {
            SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 255, 255, 255, 255));
            sharedBackgroundTextures_[monitor] = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor), surface);
            SDL_SetTextureBlendMode(sharedBackgroundTextures_[monitor], SDL_BLENDMODE_BLEND);
            SDL_FreeSurface(surface);
        }
    }
    backgroundTexture_ = sharedBackgroundTextures_.count(monitor) ? sharedBackgroundTextures_[monitor] : nullptr;
}


void Component::deInitializeFonts()
{
}


void Component::initializeFonts()
{
}

const std::string& Component::getAnimationRequestedType() const {
    return newRequests_.empty() ? animationType_ : newRequests_.back();
}

void Component::triggerEvent(const std::string_view& event, int menuIndex) {
    // Updated: Push to queue. If multiple events hit this frame, 
    // the latest one will be processed in update().
    newRequests_.emplace_back(event);
    menuIndex_ = (menuIndex > 0 ? menuIndex : 0);
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

    // 1. Process New Requests (Priority)
    if (!newRequests_.empty()) {
        std::string req = newRequests_.back();
        newRequests_.clear();

        auto newTweens = tweens_->getAnimation(req, menuIndex_);
        if (newTweens) {
            animationType_ = req;
            currentAnimation_ = newTweens;
            currentTweenIndex_ = 0;
            elapsedTweenTime_ = 0;
            storeViewInfo_ = baseViewInfo;
            currentTweenComplete_ = false;

            // --- THE PRIORITY GUARD ---
            justStartedPriority_ = true;

            // --- RELEASE THE LATCH ---
            // A valid command has arrived (e.g., onGameExit). 
            // We can now safely allow transitions and idle loops again.
            returningFromLaunch_ = false;
        }
    }

    // 2. Automated Transitions (Idle/Fallback/Looping)
    if (tweens_ && currentTweenComplete_ && !justStartedPriority_) {

        // --- THE LAUNCH LOCK ---
        if (page.getIsLaunched() && baseViewInfo.Monitor == 0) {
            // Set the latch so we know we were recently in a game.
            returningFromLaunch_ = true;
            return false;
        }

        // --- THE RETURN LATCH GUARD ---
        // If we were just in a game (returningFromLaunch_ == true) but Section 1
        // hasn't received a new command yet, stay frozen. This kills the race condition.
        if (returningFromLaunch_ && baseViewInfo.Monitor == 0) {
            return false;
        }

        if (page.isMenuScrolling()) {
            return false;
        }

        auto idleTweens = tweens_->getAnimation("idle", menuIndex_);

        if ((!idleTweens || idleTweens->size() == 0) && !page.isMenuScrolling()) {
            idleTweens = tweens_->getAnimation("menuIdle", menuIndex_);
        }

        if (idleTweens && idleTweens->size() > 0) {
            animationType_ = (tweens_->getAnimation("idle", menuIndex_) ? "idle" : "menuIdle");

            currentAnimation_ = idleTweens;
            currentTweenIndex_ = 0;
            elapsedTweenTime_ = 0;
            storeViewInfo_ = baseViewInfo;
            currentTweenComplete_ = false;
        }
    }

    // 3. Execution
    if (!currentTweenComplete_ && currentAnimation_) {
        currentTweenComplete_ = animate();
        if (currentTweenComplete_) {
            currentAnimation_ = nullptr;
            currentTweenIndex_ = 0;
        }
    }

    // --- RESET THE GUARD ---
    justStartedPriority_ = false;

    return currentTweenComplete_;
}

// used to draw lines in the layout using <container>
void Component::draw()
{
    if (backgroundTexture_ && baseViewInfo.Alpha > 0.0f && baseViewInfo.BackgroundAlpha > 0.0f) {
        SDL_FRect rect = { 0,0,0,0 };
        rect.h = baseViewInfo.ScaledHeight();
        rect.w = baseViewInfo.ScaledWidth();
        rect.x = baseViewInfo.XRelativeToOrigin();
        rect.y = baseViewInfo.YRelativeToOrigin();


        SDL_SetTextureColorMod(backgroundTexture_,
            static_cast<Uint8>(baseViewInfo.BackgroundRed * 255),
            static_cast<Uint8>(baseViewInfo.BackgroundGreen * 255),
            static_cast<Uint8>(baseViewInfo.BackgroundBlue * 255));

        SDL::renderCopyF(backgroundTexture_, baseViewInfo.BackgroundAlpha, nullptr, &rect, baseViewInfo, page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
    }
}

bool Component::animate() {
    // 1. Safety check: Ensure the pointer is valid and we are within bounds
    if (currentAnimation_ == nullptr || currentTweenIndex_ >= currentAnimation_->size()) {
        return true;
    }

    bool currentDone = true;
    double maxDurationInSet = 0.0;

    // 2. Access the TweenSet using the index-based operator[]
    TweenSet& tweens = (*currentAnimation_)[currentTweenIndex_];

    for (size_t i = 0; i < tweens.size(); i++) {
        // 3. Access each Tween by index
        const Tween& tween = tweens[i];

        if (!tween.matchesPlaylist(playlistName)) {
            continue;
        }

        maxDurationInSet = std::max(maxDurationInSet, (double)tween.duration);
        double elapsedTime = elapsedTweenTime_;
        if (elapsedTime < tween.duration) {
            currentDone = false;
        }
        else {
            elapsedTime = tween.duration;
        }

        // Apply animation logic (Note: using '.' because 'tween' is a reference)
        switch (tween.property) {
            case TWEEN_PROPERTY_X:
            baseViewInfo.X = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.X);
            break;
            case TWEEN_PROPERTY_Y:
            baseViewInfo.Y = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.Y);
            break;
            case TWEEN_PROPERTY_HEIGHT:
            baseViewInfo.Height = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.Height);
            break;
            case TWEEN_PROPERTY_WIDTH:
            baseViewInfo.Width = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.Width);
            break;
            case TWEEN_PROPERTY_ANGLE:
            baseViewInfo.Angle = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.Angle);
            break;
            case TWEEN_PROPERTY_ALPHA:
            baseViewInfo.Alpha = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.Alpha);
            break;
            case TWEEN_PROPERTY_X_ORIGIN:
            baseViewInfo.XOrigin = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.XOrigin);
            break;
            case TWEEN_PROPERTY_Y_ORIGIN:
            baseViewInfo.YOrigin = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.YOrigin);
            break;
            case TWEEN_PROPERTY_X_OFFSET:
            baseViewInfo.XOffset = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.XOffset);
            break;
            case TWEEN_PROPERTY_Y_OFFSET:
            baseViewInfo.YOffset = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.YOffset);
            break;
            case TWEEN_PROPERTY_FONT_SIZE:
            baseViewInfo.FontSize = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.FontSize);
            break;
            case TWEEN_PROPERTY_BACKGROUND_ALPHA:
            baseViewInfo.BackgroundAlpha = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.BackgroundAlpha);
            break;
            case TWEEN_PROPERTY_MAX_WIDTH:
            baseViewInfo.MaxWidth = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.MaxWidth);
            break;
            case TWEEN_PROPERTY_MAX_HEIGHT:
            baseViewInfo.MaxHeight = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.MaxHeight);
            break;
            case TWEEN_PROPERTY_LAYER:
            baseViewInfo.Layer = static_cast<unsigned int>(tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, static_cast<float>(storeViewInfo_.Layer)));
            break;
            case TWEEN_PROPERTY_CONTAINER_X:
            baseViewInfo.ContainerX = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.ContainerX);
            break;
            case TWEEN_PROPERTY_CONTAINER_Y:
            baseViewInfo.ContainerY = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.ContainerY);
            break;
            case TWEEN_PROPERTY_CONTAINER_WIDTH:
            baseViewInfo.ContainerWidth = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.ContainerWidth);
            break;
            case TWEEN_PROPERTY_CONTAINER_HEIGHT:
            baseViewInfo.ContainerHeight = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.ContainerHeight);
            break;
            case TWEEN_PROPERTY_VOLUME:
            baseViewInfo.Volume = tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, storeViewInfo_.Volume);
            break;
            case TWEEN_PROPERTY_MONITOR:
            baseViewInfo.Monitor = static_cast<unsigned int>(tween.startDefined ? tween.animate(elapsedTime) : tween.animate(elapsedTime, static_cast<float>(storeViewInfo_.Monitor)));
            break;
            case TWEEN_PROPERTY_RESTART:
            baseViewInfo.Restart = (tween.duration != 0.0f) && (elapsedTime == 0.0);
            break;
            case TWEEN_PROPERTY_NOP:
            default:
            break;
        }
    }

    if (currentDone) {
        currentTweenIndex_++;

        // Carry remainder forward for sub-frame timing accuracy
        if (maxDurationInSet > 0.0) {
            elapsedTweenTime_ -= maxDurationInSet;
            if (elapsedTweenTime_ < 0.0) elapsedTweenTime_ = 0.0;
        }
        else {
            elapsedTweenTime_ = 0.0;
        }

        storeViewInfo_ = baseViewInfo;
    }

    // Use the pointer to check the size for completion
    return (currentTweenIndex_ >= currentAnimation_->size());
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