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

#include "Component/Image.h"
#include "FontCache.h"
#include <SDL2/SDL.h>
#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#elif __has_include(<SDL2_mixer/SDL_mixer.h>)
#include <SDL2_mixer/SDL_mixer.h>
#else
#error "Cannot find SDL_mixer header"
#endif
#include <pugixml.hpp>
#include <vector>

static const int MENU_INDEX_HIGH = 16;

class ScrollingList;
class Page;
class ViewInfo;
class Configuration;
class FontManager;

class PageBuilder
{
public:
    PageBuilder(const std::string& layoutKey, const std::string& layoutPage, Configuration &c, FontCache *fc, bool isMenu = false);
    virtual ~PageBuilder();
    Page *buildPage( const std::string& collectionName = "", bool defaultToCurrentLayout = false);

private:
    std::string layoutKey;
    std::string layoutPage;
    std::string layoutPath;
    Configuration &config_;
    int screenHeight_{ 0 };
    int screenWidth_{ 0 };
    int layoutHeight_{ 0 };
    int layoutWidth_{ 0 };
    int monitor_{ 0 };
    SDL_Color fontColor_;
    std::string fontName_;
    int fontSize_{ 24 };
	bool fontGradient_{ false };
	int fontOutline_{ 0 };
    FontCache *fontCache_;
    bool isMenu_;

    FontManager *addFont(const pugi::xml_node component, const pugi::xml_node defaults, int monitor);
    void loadReloadableImages(const pugi::xml_node layout, const std::string& tagName, Page *page);
    float getVerticalAlignment(const pugi::xml_attribute attribute, float valueIfNull) const;
    float getHorizontalAlignment(const pugi::xml_attribute attribute, float valueIfNull) const;
    void buildViewInfo(pugi::xml_node componentXml, ViewInfo &info, pugi::xml_node defaultXml = pugi::xml_node{});
    bool buildComponents(pugi::xml_node layout, Page *page, const std::string&);
    void loadTweens(Component *c, pugi::xml_node componentXml);
    std::shared_ptr<AnimationEvents> createTweenInstance(pugi::xml_node componentXml);
    void buildTweenSet(AnimationEvents *tweens, pugi::xml_node componentXml, const std::string& tagName, const std::string& tweenName);
    ScrollingList * buildMenu(pugi::xml_node menuXml, Page &p, int monitor);
    void buildCustomMenu(ScrollingList *menu, const pugi::xml_node menuXml, pugi::xml_node itemDefaults);
    void buildVerticalMenu(ScrollingList *menu, const pugi::xml_node menuXml, pugi::xml_node itemDefaults);
    int parseMenuPosition(const std::string& strIndex);
    pugi::xml_attribute findAttribute(const pugi::xml_node componentXml, const std::string& attribute, const pugi::xml_node defaultXml);
    void getTweenSet(const pugi::xml_node node, Animation *animation);
    void getAnimationEvents(const pugi::xml_node node, TweenSet &tweens);
    ViewInfo * createMenuItemInfo(pugi::xml_node component, pugi::xml_node defaults, const ViewInfo& info);
};
