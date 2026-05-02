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

    // Clear the locally owned animation data to release contiguous vector memory
    currentAnimation_.Clear();

    currentTweenIndex_ = 0;
    currentTweenComplete_ = true;
    elapsedTweenTime_ = 0;

    backgroundTexture_ = nullptr;
}

// used to draw lines in the layout using <container>
void Component::allocateGraphicsMemory() {
    int monitor = baseViewInfo.Monitor;

    // --- SHARED TEXTURE LOGIC ---
    if (sharedBackgroundTextures_.find(monitor) == sharedBackgroundTextures_.end()) {

        SDL_Surface* surface = SDL_CreateRGBSurface(0, 4, 4, 32, 0, 0, 0, 0);
        SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 255, 255, 255, 255));
        sharedBackgroundTextures_[monitor] = SDL_CreateTextureFromSurface(SDL::getRenderer(monitor), surface);
        SDL_SetTextureBlendMode(sharedBackgroundTextures_[monitor], SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(surface);
    }

    // Point this component's local pointer to the shared master texture
    backgroundTexture_ = sharedBackgroundTextures_[monitor];
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

    // 1. Check for newly requested animations (e.g., from triggerEvent)
    if (animationRequested_ && !animationRequestedType_.empty() && tweens_) {
        Animation* newTweens = tweens_->getAnimation(animationRequestedType_, menuIndex_);

        if (newTweens && newTweens->size() > 0) {
            animationType_ = animationRequestedType_;
            currentAnimation_ = *newTweens;  // DEEP COPY: Ensures local ownership
            currentTweenIndex_ = 0;
            elapsedTweenTime_ = 0;
            storeViewInfo_ = baseViewInfo;
            currentTweenComplete_ = false;
        }
        animationRequested_ = false;
    }

    // 2. Handle transitions to Idle state if nothing is running
    if (tweens_ && currentTweenComplete_) {
        animationType_ = "idle";
        Animation* idleTweens = tweens_->getAnimation("idle", menuIndex_);

        if (idleTweens && idleTweens->size() == 0 && !page.isMenuScrolling()) {
            idleTweens = tweens_->getAnimation("menuIdle", menuIndex_);
        }

        if (idleTweens && idleTweens->size() > 0) {
            currentAnimation_ = *idleTweens; // DEEP COPY
            currentTweenIndex_ = 0;
            elapsedTweenTime_ = 0;
            storeViewInfo_ = baseViewInfo;
            currentTweenComplete_ = false;
        }
    }

    // 3. Process the animation frame
    if (!currentTweenComplete_) {
        currentTweenComplete_ = animate();
        if (currentTweenComplete_) {
            currentAnimation_.Clear(); // Free memory once finished
            currentTweenIndex_ = 0;
        }
    }

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
            static_cast<char>(baseViewInfo.BackgroundRed * 255),
            static_cast<char>(baseViewInfo.BackgroundGreen * 255),
            static_cast<char>(baseViewInfo.BackgroundBlue * 255));

        SDL::renderCopyF(backgroundTexture_, baseViewInfo.BackgroundAlpha, nullptr, &rect, baseViewInfo, page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
    }
}

bool Component::animate() {
    // Check if we have any animation data to process
    if (currentAnimation_.size() == 0 || currentTweenIndex_ >= currentAnimation_.size()) {
        return true; // Animation is finished or empty
    }

    bool currentDone = true;
    // Get the current TweenSet from the contiguous vector
    TweenSet* tweens = currentAnimation_.tweenSet(currentTweenIndex_);
    if (!tweens) return true;

    for (unsigned int i = 0; i < tweens->size(); i++) {
        const Tween* tween = tweens->getTween(i);

        // Check if this specific tween is filtered out for the current playlist
        if (!tween->matchesPlaylist(playlistName)) {
            continue;
        }

        double elapsedTime = elapsedTweenTime_;
        if (elapsedTime < tween->duration) {
            currentDone = false;
        }
        else {
            elapsedTime = tween->duration;
        }

        // Apply animation logic based on whether a start value is defined in the XML
        switch (tween->property) {
            case TWEEN_PROPERTY_X:
            baseViewInfo.X = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.X);
            break;
            case TWEEN_PROPERTY_Y:
            baseViewInfo.Y = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.Y);
            break;
            case TWEEN_PROPERTY_HEIGHT:
            baseViewInfo.Height = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.Height);
            break;
            case TWEEN_PROPERTY_WIDTH:
            baseViewInfo.Width = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.Width);
            break;
            case TWEEN_PROPERTY_ANGLE:
            baseViewInfo.Angle = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.Angle);
            break;
            case TWEEN_PROPERTY_ALPHA:
            baseViewInfo.Alpha = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.Alpha);
            break;
            case TWEEN_PROPERTY_X_ORIGIN:
            baseViewInfo.XOrigin = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.XOrigin);
            break;
            case TWEEN_PROPERTY_Y_ORIGIN:
            baseViewInfo.YOrigin = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.YOrigin);
            break;
            case TWEEN_PROPERTY_X_OFFSET:
            baseViewInfo.XOffset = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.XOffset);
            break;
            case TWEEN_PROPERTY_Y_OFFSET:
            baseViewInfo.YOffset = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.YOffset);
            break;
            case TWEEN_PROPERTY_FONT_SIZE:
            baseViewInfo.FontSize = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.FontSize);
            break;
            case TWEEN_PROPERTY_BACKGROUND_ALPHA:
            baseViewInfo.BackgroundAlpha = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.BackgroundAlpha);
            break;
            case TWEEN_PROPERTY_MAX_WIDTH:
            baseViewInfo.MaxWidth = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.MaxWidth);
            break;
            case TWEEN_PROPERTY_MAX_HEIGHT:
            baseViewInfo.MaxHeight = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.MaxHeight);
            break;
            case TWEEN_PROPERTY_LAYER:
            baseViewInfo.Layer = static_cast<unsigned int>(tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, static_cast<float>(storeViewInfo_.Layer)));
            break;
            case TWEEN_PROPERTY_CONTAINER_X:
            baseViewInfo.ContainerX = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.ContainerX);
            break;
            case TWEEN_PROPERTY_CONTAINER_Y:
            baseViewInfo.ContainerY = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.ContainerY);
            break;
            case TWEEN_PROPERTY_CONTAINER_WIDTH:
            baseViewInfo.ContainerWidth = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.ContainerWidth);
            break;
            case TWEEN_PROPERTY_CONTAINER_HEIGHT:
            baseViewInfo.ContainerHeight = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.ContainerHeight);
            break;
            case TWEEN_PROPERTY_VOLUME:
            baseViewInfo.Volume = tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, storeViewInfo_.Volume);
            break;
            case TWEEN_PROPERTY_MONITOR:
            baseViewInfo.Monitor = static_cast<unsigned int>(tween->startDefined ? tween->animate(elapsedTime) : tween->animate(elapsedTime, static_cast<float>(storeViewInfo_.Monitor)));
            break;
            case TWEEN_PROPERTY_RESTART:
            baseViewInfo.Restart = (tween->duration != 0.0f) && (elapsedTime == 0.0);
            break;
            case TWEEN_PROPERTY_NOP:
            default:
            break;
        }
    }

    // If all tweens in the current set are done, move to the next set
    if (currentDone) {
        currentTweenIndex_++;
        elapsedTweenTime_ = 0;
        storeViewInfo_ = baseViewInfo;
    }

    // Return true if we have completed all sets in the animation
    return (currentTweenIndex_ >= currentAnimation_.size());
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