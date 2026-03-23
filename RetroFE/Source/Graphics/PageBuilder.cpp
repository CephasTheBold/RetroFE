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

#include "PageBuilder.h"
#include "Page.h"
#include "ViewInfo.h"
#include "Component/Container.h"
#include "Component/Image.h"
#include "Component/Text.h"
#include "Component/ReloadableText.h"
#include "Component/ReloadableMedia.h"
#include "Component/ReloadableScrollingText.h"
#include "Component/ReloadableHiscores.h"
#include "Component/ReloadableGlobalHiscores.h"
#include "Component/ScrollingList.h"
#include "Component/VideoBuilder.h"
#include "Component/MusicPlayerComponent.h"
#include "Animate/AnimationEvents.h"
#include "Animate/TweenTypes.h"
#include "../Sound/Sound.h"
#include "../Collection/Item.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include "../Database/GlobalOpts.h"
#include <algorithm>
#include <cfloat>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <map>
#include <filesystem>
#include <memory>

#include <pugixml.hpp>

using namespace pugi;

// Pre-process a raw XML buffer to fix mismatched opening/closing tag names.
//
// rapidxml never validated that a closing tag's name matched its corresponding
// opening tag — it simply popped the element stack on any </...>.  pugixml is
// strict and returns status_end_element_mismatch for the same files.
//
// This function replicates rapidxml's lenient behaviour: it walks the raw bytes
// of the buffer, tracks the stack of open element names, and rewrites every
// closing tag whose name does not match the top of the stack so that it uses the
// correct name instead.  The result is a buffer that pugixml will accept without
// errors, and that is semantically equivalent to what rapidxml used to produce.
static void fixMismatchedClosingTags(std::vector<char>& buf) {
	std::size_t n = buf.size();
	std::vector<std::string> stack; // stack of open element names

	auto skipWs = [&](std::size_t i) {
		while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) ++i;
		return i;
	};

	std::size_t i = 0;
	while (i < n) {
		if (buf[i] != '<') { ++i; continue; }
		++i; // past '<'
		if (i >= n) break;

		if (buf[i] == '!') {
			// Comment (<!--) or declaration/CDATA
			if (i + 2 < n && buf[i + 1] == '-' && buf[i + 2] == '-') {
				i += 3; // past '<!--'
				while (i + 2 < n && !(buf[i] == '-' && buf[i + 1] == '-' && buf[i + 2] == '>')) ++i;
				i += 3; // past '-->'
			}
			else {
				while (i < n && buf[i] != '>') ++i;
				if (i < n) ++i;
			}
		}
		else if (buf[i] == '?') {
			// Processing instruction
			while (i + 1 < n && !(buf[i] == '?' && buf[i + 1] == '>')) ++i;
			i += 2;
		}
		else if (buf[i] == '/') {
			// Closing tag: </tagname ...>
			++i; // past '/'
			i = skipWs(i);

			std::size_t nameBegin = i;
			while (i < n && buf[i] != '>' && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n') ++i;
			std::size_t nameEnd = i;

			// Advance past '>'
			while (i < n && buf[i] != '>') ++i;
			if (i < n) ++i;

			if (stack.empty()) continue;

			std::string closingName(buf.data() + nameBegin, nameEnd - nameBegin);
			const std::string& expected = stack.back();

			if (closingName != expected) {
				// Replace the wrong closing-tag name with the expected one.
				std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(expected.size()) -
					static_cast<std::ptrdiff_t>(closingName.size());
				if (delta == 0) {
					std::memcpy(buf.data() + nameBegin, expected.data(), expected.size());
				}
				else {
					buf.erase(buf.begin() + nameBegin, buf.begin() + nameEnd);
					buf.insert(buf.begin() + nameBegin, expected.begin(), expected.end());
					n = buf.size();
					i = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) + delta);
				}
			}
			stack.pop_back();
		}
		else {
			// Opening tag: <tagname ...> or self-closing <tagname .../>
			std::size_t nameBegin = i;
			while (i < n && buf[i] != '>' && buf[i] != '/' &&
				buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n') ++i;
			std::string name(buf.data() + nameBegin, i - nameBegin);

			if (name.empty()) {
				while (i < n && buf[i] != '>') ++i;
				if (i < n) ++i;
				continue;
			}

			// Scan to end of tag, respecting quoted attribute values, to detect />
			bool selfClose = false;
			bool inQuote = false;
			char quoteChar = 0;
			while (i < n) {
				char c = buf[i];
				if (inQuote) {
					if (c == quoteChar) inQuote = false;
				}
				else if (c == '"' || c == '\'') {
					inQuote = true; quoteChar = c;
				}
				else if (c == '/' && i + 1 < n && buf[i + 1] == '>') {
					selfClose = true; i += 2; break;
				}
				else if (c == '>') {
					++i; break;
				}
				++i;
			}

			if (!selfClose) stack.push_back(std::move(name));
		}
	}
}

static const int MENU_FIRST = 0;   // first visible item in the list
static const int MENU_LAST = -3;   // last visible item in the list
static const int MENU_START = -1;  // first item transitions here after it scrolls "off the menu/screen"
static const int MENU_END = -2;    // last item transitions here after it scrolls "off the menu/screen"
//static const int MENU_CENTER = -4;

//todo: this file is starting to become a god class of building. Consider splitting into sub-builders
PageBuilder::PageBuilder(const std::string& layoutKey, const std::string& layoutPage, Configuration& c, FontCache* fc, bool isMenu)
	: layoutKey(layoutKey)
	, layoutPage(layoutPage)
	, config_(c)
	, fontCache_(fc)
	, isMenu_(isMenu) {
	screenWidth_ = SDL::getWindowWidth(0);
	screenHeight_ = SDL::getWindowHeight(0);
	fontColor_.a = 255;
	fontColor_.r = 0;
	fontColor_.g = 0;
	fontColor_.b = 0;
}

PageBuilder::~PageBuilder() = default;

Page* PageBuilder::buildPage(const std::string& collectionName, bool /* defaultToCurrentLayout */) {
	Page* page = nullptr;

	std::string layoutFile;
	std::string layoutFileAspect;
	std::string layoutName = layoutKey;
	std::string layoutPathDefault = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName);
	bool fixedResLayouts = false;
	config_.getProperty(OPTION_FIXEDRESLAYOUTS, fixedResLayouts);
	namespace fs = std::filesystem;

	// These just prevent repeated logging
	bool splashInitialized = false;
	bool fixedResLayoutsInitialized = false;

	// Initialize layoutPath appropriately
	if (isMenu_) {
		layoutPath = Utils::combinePath(Configuration::absolutePath, "menu");
	}
	else if (!collectionName.empty()) {
		// Attempt to set layoutPath to the collection-specific path
		std::string collectionLayoutPath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, "collections", collectionName, "layout");

		// Check if the collection-specific layout directory exists
		if (std::filesystem::exists(collectionLayoutPath)) {
			layoutPath = collectionLayoutPath;
		}
		else {
			// If it doesn't exist, fall back to the default layout path
			layoutPath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName);
		}
	}
	else {
		// Default case, likely for splash page or general layouts
		layoutPath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName);
	}


	std::vector<std::string> layouts;
	layouts.push_back(layoutPage);
	// Add "layout - #.xml" files unless the layoutPage is "splash".
	if (layoutPage != "splash") {
		// Add "layout - #.xml" files to the list of things to parse.
		for (int i = 0; i < Page::MAX_LAYOUTS; i++) {
			layouts.push_back("layout - " + std::to_string(i));
		}
	}

	// Each iteration parses one XML file into a short-lived xml_document.
	// pugixml::load_buffer copies the file data into the document's own memory,
	// so the read buffer may be released immediately after the call.
	// The xml_document (and all xml_node handles derived from it) is destroyed at
	// the end of each iteration — after buildComponents() returns — which is safe
	// because buildComponents() copies all string values it needs into std::string
	// members of the long-lived Page and Component objects.
	for (unsigned int layoutIndex = 0; layoutIndex < layouts.size(); ++layoutIndex) {
		const std::string& currentLayoutName = layouts[layoutIndex];
		auto doc = std::make_unique<xml_document>();
		std::ifstream file;

		// Use currentLayoutPath for this iteration
		std::string currentLayoutPath = layoutPath;

		// Override layout with layoutFromAnotherCollection if specified
		std::string layoutFromAnotherCollection;
		config_.getProperty("collections." + collectionName + ".layoutFromAnotherCollection", layoutFromAnotherCollection);
		if (!layoutFromAnotherCollection.empty()) {
			LOG_INFO("Layout", "Using layout from collection: " + layoutFromAnotherCollection + " " + currentLayoutName + ".xml");
			currentLayoutPath = Utils::combinePath(layoutPathDefault, "collections", layoutFromAnotherCollection, "layout");
		}

		// Build layoutFile path using currentLayoutPath
		layoutFile = Utils::combinePath(currentLayoutPath, currentLayoutName + ".xml");

		if (fixedResLayouts) {
			// Use fixed resolution layout, e.g., layout1920x1080.xml
			if (!fixedResLayoutsInitialized) {
				LOG_INFO("Layout", "Fixed resolution layouts have been enabled");
				fixedResLayoutsInitialized = true;
			}
			std::string aspect = std::to_string(screenWidth_ / Utils::gcd(screenWidth_, screenHeight_)) + "x" +
				std::to_string(screenHeight_ / Utils::gcd(screenWidth_, screenHeight_));
			layoutFileAspect = Utils::combinePath(layoutPathDefault, aspect + currentLayoutName + ".xml");
			if (fs::exists(layoutFileAspect)) {
				layoutFile = layoutFileAspect;
			}
			else {
				LOG_ERROR("Layout", "Unable to find fixed resolution layout: " + layoutFileAspect);
				exit(EXIT_FAILURE);
			}
		}

		// Check if the layout file exists
		if (fs::exists(layoutFile)) {
			LOG_INFO("Layout", "Attempting to initialize layout: " + layoutFile);
		}
		else {
			// Handle special cases when layout file does not exist
			if (layoutPath != layoutPathDefault) {
				if (currentLayoutName != "splash") {
					layoutFile = Utils::combinePath(layoutPathDefault, currentLayoutName + ".xml");
					if (fs::exists(layoutFile)) {
						LOG_INFO("Layout", "Attempting to initialize default layout: " + layoutFile);
					}
					else {
						LOG_WARNING("Layout", "Layout not found: " + layoutFile);
						continue; // Skip to next layout
					}
				}
				else if (!splashInitialized && fs::exists(Utils::combinePath(layoutPathDefault, "splash.xml"))) {
					layoutFile = Utils::combinePath(layoutPathDefault, "splash.xml");
					LOG_INFO("Layout", "Attempting to initialize splash: " + layoutFile);
					splashInitialized = true;
				}
				else {
					LOG_WARNING("Layout", "Layout file does not exist: " + layoutFile);
					continue; // Skip to next layout
				}
			}
			else {
				LOG_WARNING("Layout", "Layout file does not exist: " + layoutFile);
				continue; // Skip to next layout
			}
		}

		// Try to open the file
		file.open(layoutFile.c_str());
		if (!file.is_open()) {
			LOG_ERROR("Layout", "Failed to open file: " + layoutFile);
			continue; // Skip to next layout
		}

		// Read the file into buffer
		std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		file.close();

		// Compatibility fix for layout files authored when the project used rapidxml, which
		// silently accepted a missing space between a closing attribute-value quote and the
		// start of the next attribute name (e.g. type="xoffset"to="0").  pugixml requires
		// strict XML, so we insert the missing space here before parsing.
		// Only a closing quote (not preceded by '=') immediately followed by an XML
		// name-start character can be an attribute-separator boundary.
		auto isXmlNameStartChar = [](char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
		};
		for (std::size_t i = 0; i + 1 < buffer.size(); ++i) {
			if (buffer[i] == '"' && i > 0 && buffer[i - 1] != '=' && isXmlNameStartChar(buffer[i + 1])) {
				buffer.insert(buffer.begin() + i + 1, ' ');
				++i; // skip the inserted space
			}
		}

		// Compatibility fix for layout files authored when the project used rapidxml, which
		// never validated that a closing tag's name matched the opening tag — it simply
		// popped the element stack on any </...>.  pugixml is strict and returns
		// status_end_element_mismatch for such files.  fixMismatchedClosingTags() replicates
		// rapidxml's lenient behaviour by rewriting every wrong </closingTag> to use the
		// correct name from the element stack.
		fixMismatchedClosingTags(buffer);

		try {
			xml_parse_result parseResult = doc->load_buffer(buffer.data(), buffer.size());
			if (!parseResult) {
				auto line = static_cast<long>(std::count(buffer.begin(), buffer.begin() + parseResult.offset, char('\n')) + 1);
				std::stringstream ss;
				ss << "Could not parse layout file. [Line: " << line << "] in " << layoutFile << " Reason: " << parseResult.description();
				LOG_ERROR("Layout", ss.str());
				continue; // Skip to next layout
			}
			xml_node root = doc->child("layout");

			if (!root) {
				LOG_ERROR("Layout", "Missing <layout> tag in " + layoutFile);
				continue; // Skip to next layout
			}

			// Process layout attributes
			xml_attribute layoutWidthXml = root.attribute("width");
			xml_attribute layoutHeightXml = root.attribute("height");
			xml_attribute fontXml = root.attribute("font");
			xml_attribute fontColorXml = root.attribute("fontColor");
			xml_attribute fontSizeXml = root.attribute("loadFontSize");
			xml_attribute fontGradientXml = root.attribute("fontGradient");
			xml_attribute fontOutlineXml = root.attribute("fontOutline");
			xml_attribute minShowTimeXml = root.attribute("minShowTime");
			xml_attribute controls = root.attribute("controls");
			xml_attribute layoutMonitorXml = root.attribute("monitor");

			// Process font attributes
			if (fontXml) {
				fontName_ = config_.convertToAbsolutePath(
					Utils::combinePath(config_.absolutePath, "layouts", layoutKey, ""),
					fontXml.value());

				// Check if the font file exists
				if (!std::filesystem::exists(fontName_)) {
					LOG_ERROR("RetroFE", "Specified font at \n    " + fontName_ + "\n does not exist. Falling back to standard font.");
					fontName_ = config_.convertToAbsolutePath(
						Utils::combinePath(config_.absolutePath, "layouts", layoutKey, ""),
						"fonts/standard.ttf");
				}
			}

			// Process font color
			if (fontColorXml) {
				int intColor = 0;
				std::stringstream ss;
				ss << std::hex << fontColorXml.value();
				ss >> intColor;

				fontColor_.b = intColor & 0xFF;
				intColor >>= 8;
				fontColor_.g = intColor & 0xFF;
				intColor >>= 8;
				fontColor_.r = intColor & 0xFF;
			}

			// Process font size
			if (fontSizeXml) {
				fontSize_ = fontSizeXml.as_int();
			}

			if (fontGradientXml) {
				fontGradient_ = fontGradientXml.as_bool();
			}

			if (fontOutlineXml) {
				fontOutline_ = fontOutlineXml.as_int();
			}

			// Process layout dimensions
			if (layoutWidthXml && layoutHeightXml) {
				std::string layoutWidthStr = layoutWidthXml.value();
				std::string layoutHeightStr = layoutHeightXml.value();

				// Determine the monitor from the layout tag or use a default
				int monitor = layoutMonitorXml.as_int(monitor_);

				// Get the width and height based on whether "stretch" is specified
				if (layoutWidthStr == "stretch") {
					layoutWidth_ = SDL::getWindowWidth(monitor);
				}
				else {
					layoutWidth_ = layoutWidthXml.as_int();
				}

				if (layoutHeightStr == "stretch") {
					layoutHeight_ = SDL::getWindowHeight(monitor);
				}
				else {
					layoutHeight_ = layoutHeightXml.as_int();
				}

				if (layoutWidth_ != 0 && layoutHeight_ != 0) {
					std::stringstream ss;
					ss << layoutWidth_ << "x" << layoutHeight_ << " (scale "
						<< static_cast<float>(SDL::getWindowWidth(monitor)) / layoutWidth_ << "x"
						<< static_cast<float>(SDL::getWindowHeight(monitor)) / layoutHeight_ << ")";
					LOG_INFO("Layout", "Layout resolution " + ss.str());

					if (!page)
						page = new Page(config_, layoutWidth_, layoutHeight_);
					else {
						page->setLayoutWidth(layoutIndex, layoutWidth_);
						page->setLayoutHeight(layoutIndex, layoutHeight_);

						if (monitor) {
							page->setLayoutWidthByMonitor(monitor, layoutWidth_);
							page->setLayoutHeightByMonitor(monitor, layoutHeight_);
						}
					}
				}
			}

			// Guard: if no page has been created yet (e.g. the layout file omits width/height),
			// there is nothing to apply subsequent attributes or components to.  This preserves
			// the same return-value contract as the original rapidxml implementation: the caller
			// always receives the Page* created by the first valid layout file, or nullptr if no
			// file could establish dimensions.  Without this guard the dereferences below would
			// be undefined behaviour (null pointer crash) when page is still null.
			if (!page) {
				LOG_WARNING("Layout", "Layout \"" + layoutFile + "\" is missing required width and height attributes; please add them to the <layout> tag. Skipping attribute and component processing for this file.");
				continue;
			}

			// Process minShowTime
			if (minShowTimeXml) {
				page->setMinShowTime(minShowTimeXml.as_float());
			}

			// Process controls
			// Note: pugixml attribute.value() always returns a valid C-string (never nullptr),
			// so only the existence check and the non-empty string check are needed.
			if (controls && controls.value()[0] != '\0') {
				std::string controlLayout = controls.value();
				LOG_INFO("Layout", "Layout set custom control type " + controlLayout);
				page->setControlsType(controlLayout);
			}

			// Load sounds
			for (xml_node sound = root.child("sound"); sound; sound = sound.next_sibling("sound")) {
				xml_attribute src = sound.attribute("src");
				xml_attribute type = sound.attribute("type");
				if (!src || !type) {
					LOG_ERROR("Layout", "Sound tag missing 'src' or 'type' attribute");
					continue;
				}
				std::string file = Configuration::convertToAbsolutePath(layoutPath, src.value());

				// Check if collection's assets are in a different theme
				std::string layoutNameOverride;
				config_.getProperty("collections." + collectionName + ".layout", layoutNameOverride);
				if (layoutNameOverride.empty()) {
					config_.getProperty(OPTION_LAYOUT, layoutNameOverride);
				}
				std::string altfile = Utils::combinePath(Configuration::absolutePath, "layouts", layoutNameOverride, std::string(src.value()));

				auto* soundObj = new Sound(file, altfile);
				std::string soundType = type.value();

				if (soundType == "load") {
					page->setLoadSound(soundObj);
				}
				else if (soundType == "unload") {
					page->setUnloadSound(soundObj);
				}
				else if (soundType == "highlight") {
					page->setHighlightSound(soundObj);
				}
				else if (soundType == "select") {
					page->setSelectSound(soundObj);
				}
				else {
					LOG_WARNING("Layout", "Unsupported sound effect type \"" + soundType + "\"");
				}
			}

			// Build components.
			// Note: buildComponents() always returns true (every failure path within it logs a
			// warning/error and moves on rather than propagating a false return).  The failure
			// branch below is therefore currently unreachable, but is retained as a defensive
			// guard in case that contract changes in the future.
			if (!buildComponents(root, page, collectionName)) {
				LOG_ERROR("Layout", "Failed to build components for layout: " + layoutFile);
				delete page;
				page = nullptr;
				continue; // Skip to next layout
			}

			// Successfully built the page
			if (fixedResLayouts) {
				LOG_INFO("Layout", "Initialized " + layoutFileAspect);
			}
			else {
				LOG_INFO("Layout", "Initialized " + layoutFile);
			}

		}
		catch (std::exception& e) {
			LOG_ERROR("Layout", "Exception while parsing layout file: " + std::string(e.what()));
			continue; // Skip to next layout
		}
	}

	if (!page) {
		LOG_ERROR("Layout", "Could not initialize any layouts");
	}

	return page;
}
float PageBuilder::getHorizontalAlignment(const xml_attribute attribute, float valueIfNull) const {
	float value;
	std::string str;

	if (!attribute) {
		value = valueIfNull;
	}
	else {
		str = attribute.value();

		if (str.empty()) {
			// Handle the case where the attribute value is an empty string
			value = valueIfNull; // Or any default value you'd prefer
		}
		else if (!str.compare("left")) {
			value = 0;
		}
		else if (!str.compare("center")) {
			value = static_cast<float>(layoutWidth_) / 2;
		}
		else if (!str.compare("right") || !str.compare("stretch")) {
			value = static_cast<float>(layoutWidth_);
		}
		else if (str.back() == '%') {
			float percent = Utils::convertFloat(str.substr(0, str.length() - 1));
			value = std::round(static_cast<float>(layoutWidth_) * (percent / 100.0f));
		}
		else {
			value = Utils::convertFloat(str);
		}
	}

	return value;
}

float PageBuilder::getVerticalAlignment(const xml_attribute attribute, float valueIfNull) const {
	float value;
	std::string str;

	if (!attribute) {
		value = valueIfNull;
	}
	else {
		str = attribute.value();

		if (str.empty()) {
			// Handle the case where the attribute value is an empty string
			value = valueIfNull; // Or any default value you'd prefer
		}
		else if (!str.compare("top")) {
			value = 0;
		}
		else if (!str.compare("center")) {
			value = static_cast<float>(layoutHeight_) / 2;
		}
		else if (!str.compare("bottom") || !str.compare("stretch")) {
			value = static_cast<float>(layoutHeight_);
		}
		else if (str.back() == '%') {
			std::string_view percentStr(str.data(), str.length() - 1);
			float percent = Utils::convertFloat(percentStr);
			value = std::round(static_cast<float>(layoutHeight_) * (percent / 100.0f));
		}
		else {
			value = Utils::convertFloat(str);
		}
	}

	return value;
}

bool PageBuilder::buildComponents(xml_node layout, Page* page, const std::string& collectionName)

{
	xml_attribute layoutMonitorXml = layout.attribute("monitor");
	int layoutMonitor = layoutMonitorXml.as_int(monitor_);
	// Check if the specified monitor exists (for this "layout")
	if (layoutMonitor + 1 > SDL::getScreenCount()) {
		LOG_WARNING("Layout", "Skipping layout due to non-existent monitor index: " + std::to_string(layoutMonitor));
		return true; // Skip this layout
	}

	for (xml_node componentXml = layout.child("menu"); componentXml; componentXml = componentXml.next_sibling("menu")) {
		// Extract "monitor" attribute specifically for this "menu" node
		xml_attribute menuMonitorXml = componentXml.attribute("monitor");
		int menuMonitor = menuMonitorXml.as_int(layoutMonitor);
		// Check if the specified monitor exists (for this "menu")
		if (menuMonitor + 1 > SDL::getScreenCount()) {
			LOG_WARNING("Layout", "Skipping menu due to non-existent monitor index: " + std::to_string(menuMonitor));
			continue; // Skip this menu and go to the next
		}

		// If the monitor exists, proceed to build the menu
		ScrollingList* scrollingList = buildMenu(componentXml, *page, menuMonitor);
		xml_attribute indexXml = componentXml.attribute("menuIndex");
		int index = indexXml.as_int(-1);
		if (scrollingList && scrollingList->isPlaylist()) {
			page->setPlaylistMenu(scrollingList);
		}
		if (scrollingList) {
			page->pushMenu(scrollingList, index);
		}
	}

	for (xml_node componentXml = layout.child("container"); componentXml; componentXml = componentXml.next_sibling("container")) {
		auto* c = new Container(*page);
		if (auto const menuScrollReload = componentXml.attribute("menuScrollReload");
			menuScrollReload.as_bool()) {
			c->setMenuScrollReload(true);
		}
		xml_attribute monitorXml = componentXml.attribute("monitor");
		c->baseViewInfo.Monitor = monitorXml.as_int(layoutMonitor);
		c->baseViewInfo.Layout = page->getCurrentLayout();

		buildViewInfo(componentXml, c->baseViewInfo);
		loadTweens(c, componentXml);
		page->addComponent(c);
	}
	for (xml_node componentXml = layout.child("image"); componentXml; componentXml = componentXml.next_sibling("image")) {
		xml_attribute src = componentXml.attribute("src");
		xml_attribute idXml = componentXml.attribute("id");
		xml_attribute monitorXml = componentXml.attribute("monitor");
		xml_attribute additiveXml = componentXml.attribute("additive");

		int id = idXml.as_int(-1);
		int imageMonitor = monitorXml.as_int(layoutMonitor);

		if (imageMonitor + 1 > SDL::getScreenCount()) {
			LOG_WARNING("Layout", "Skipping image due to non-existent monitor index: " + std::to_string(imageMonitor));
			continue;
		}

		if (!src) {
			LOG_ERROR("Layout", "Image component in layout does not specify a source image file");
		}
		else {
			std::string imageBasePath = layoutPath; // Use layoutPath as the base

			std::string layoutFromAnotherCollection;
			config_.getProperty("collections." + collectionName + ".layoutFromAnotherCollection", layoutFromAnotherCollection);
			if (!layoutFromAnotherCollection.empty()) {
				// Handle alternative layout path without modifying layoutPath
				std::string layoutPathDefault = Utils::combinePath(Configuration::absolutePath, "layouts", layoutKey);
				std::string alternativeLayoutPath = Utils::combinePath(layoutPathDefault, "collections", collectionName, "layout");
				if (std::filesystem::exists(alternativeLayoutPath)) {
					imageBasePath = alternativeLayoutPath;
				}
				else {
					std::string layoutArtworkCollectionPath = Utils::combinePath(Configuration::absolutePath, "collections", collectionName, "layout_artwork");
					LOG_INFO("Layout", "Using layout_artwork folder in: " + layoutArtworkCollectionPath);
					imageBasePath = layoutArtworkCollectionPath;
				}
			}

			// Construct the image path
			std::string imagePath = Configuration::convertToAbsolutePath(imageBasePath, src.value());

			// Alternative image path in case the asset is in a different theme
			std::string layoutName;
			config_.getProperty("collections." + collectionName + ".layout", layoutName);
			if (layoutName.empty()) {
				config_.getProperty(OPTION_LAYOUT, layoutName);
			}
			std::string altImagePath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, src.value());

			bool additive = additiveXml.as_bool();
			// Check existence of either imagePath or altImagePath
			if (!std::filesystem::exists(imagePath) && !std::filesystem::exists(altImagePath)) {
				LOG_ERROR("Layout", "Failed to find image at: " + imagePath + " or " + altImagePath);
				continue;
			}
			auto* c = new Image(imagePath, altImagePath, *page, imageMonitor, additive);
			if (c)
			{
				c->allocateGraphicsMemory();
				c->setId(id);
				if (auto const menuScrollReload = componentXml.attribute("menuScrollReload");
					menuScrollReload.as_bool()) {
					c->setMenuScrollReload(true);
				}

				// Explicitly set the monitor and layout
				c->baseViewInfo.Monitor = monitorXml.as_int(layoutMonitor);
				c->baseViewInfo.Layout = page->getCurrentLayout();

				buildViewInfo(componentXml, c->baseViewInfo);
				loadTweens(c, componentXml);
				page->addComponent(c);
			}
			else
			{
				LOG_ERROR("Layout", "Failed to load component at: " + imagePath + " or " + altImagePath);
			}
		}
	}



	for (xml_node componentXml = layout.child("video"); componentXml; componentXml = componentXml.next_sibling("video"))
	{
		xml_attribute srcXml = componentXml.attribute("src");
		xml_attribute numLoopsXml = componentXml.attribute("numLoops");
		xml_attribute idXml = componentXml.attribute("id");
		xml_attribute monitorXml = componentXml.attribute("monitor");
		xml_attribute softOverlayXml = componentXml.attribute("softOverlay"); // New attribute

		int id = -1;
		if (idXml) {
			id = idXml.as_int();
		}

		if (!srcXml) {
			LOG_ERROR("Layout", "Video component in layout does not specify a source video file");
		}
		else {
			VideoBuilder videoBuild{};
			std::string videoPath = Utils::combinePath(Configuration::convertToAbsolutePath(layoutPath, ""), std::string(srcXml.value()));

			// Check if collection's assets are in a different theme
			std::string layoutName;
			config_.getProperty("collections." + collectionName + ".layout", layoutName);
			if (layoutName.empty()) {
				config_.getProperty(OPTION_LAYOUT, layoutName);
			}

			bool softOverlay = false;
			if (softOverlayXml.as_bool()) {
				softOverlay = true;
			}

			std::string altVideoPath = Utils::combinePath(Configuration::absolutePath, "layouts", layoutName, std::string(srcXml.value()));
			int numLoops = numLoopsXml.as_int(1);

			int videoMonitor = monitorXml.as_int(layoutMonitor); // Use layout's monitor if not specified at the menu level

			// Check if the specified monitor exists (for this "image")
			if (videoMonitor + 1 > SDL::getScreenCount()) {
				LOG_WARNING("Layout", "Skipping video due to non-existent monitor index: " + std::to_string(videoMonitor));
				continue; // Skip this image and go to the next
			}

			// Don't add videos if display doesn't exist

			std::filesystem::path primaryPath(videoPath);
			std::filesystem::path altPath(altVideoPath);

			VideoComponent* c = videoBuild.createVideo(primaryPath.parent_path().string(), *page, primaryPath.stem().string(), videoMonitor, numLoops, softOverlay);

			if (!c) {
				// Try alternative video path if the primary path did not yield a VideoComponent
				c = videoBuild.createVideo(altPath.parent_path().string(), *page, altPath.stem().string(), videoMonitor, numLoops, softOverlay);
			}

			if (c) {
				c->allocateGraphicsMemory();
				c->setId(id);

				// Additional settings and configurations
				if (!componentXml.attribute("pauseOnScroll").as_bool(true)) {
					c->setPauseOnScroll(false);
				}

				if (componentXml.attribute("menuScrollReload").as_bool()) {
					c->setMenuScrollReload(true);
				}

				if (componentXml.attribute("animationDoneRemove").as_bool()) {
					c->setAnimationDoneRemove(true);
				}
				c->baseViewInfo.Monitor = monitorXml.as_int(layoutMonitor);
				c->baseViewInfo.Layout = page->getCurrentLayout();

				buildViewInfo(componentXml, c->baseViewInfo);
				loadTweens(c, componentXml);
				page->addComponent(c);
			}
			else {
				LOG_WARNING("Layout", "Could not find video file: " + videoPath + " or " + altVideoPath);
			}

		}

	}

	for (xml_node componentXml = layout.child("text"); componentXml; componentXml = componentXml.next_sibling("text")) {
		xml_attribute value = componentXml.attribute("value");
		xml_attribute idXml = componentXml.attribute("id");
		xml_attribute monitorXml = componentXml.attribute("monitor");

		int id = idXml.as_int(-1);

		if (!value) {
			LOG_WARNING("Layout", "Text component in layout does not specify a value");
		}
		else {
			int textMonitor = monitorXml.as_int(layoutMonitor);
			FontManager* font = addFont(componentXml, pugi::xml_node{}, textMonitor);

			auto* c = new Text(value.value(), *page, font, textMonitor);
			c->setId(id);
			if (componentXml.attribute("menuScrollReload").as_bool())
			{
				c->setMenuScrollReload(true);
			}

			buildViewInfo(componentXml, c->baseViewInfo);
			loadTweens(c, componentXml);
			page->addComponent(c);
		}
	}

	for (xml_node componentXml = layout.child("statusText"); componentXml; componentXml = componentXml.next_sibling("statusText")) {
		xml_attribute monitorXml = componentXml.attribute("monitor");
		int statusTextMonitor = monitorXml.as_int(layoutMonitor);
		FontManager* font = addFont(componentXml, pugi::xml_node{}, statusTextMonitor);

		auto* c = new Text("", *page, font, statusTextMonitor);
		if (componentXml.attribute("menuScrollReload").as_bool())
		{
			c->setMenuScrollReload(true);
		}

		buildViewInfo(componentXml, c->baseViewInfo);
		loadTweens(c, componentXml);
		page->addComponent(c);
		page->setStatusTextComponent(c);
	}


	loadReloadableImages(layout, "reloadableImage", page);
	loadReloadableImages(layout, "reloadableAudio", page);
	loadReloadableImages(layout, "reloadableVideo", page);
	loadReloadableImages(layout, "reloadableText", page);
	loadReloadableImages(layout, "reloadableScrollingText", page);
	loadReloadableImages(layout, "reloadableHiscores", page);
	loadReloadableImages(layout, "reloadableGlobalHiscores", page);
	loadReloadableImages(layout, "musicPlayer", page);

	return true;
}


void PageBuilder::loadReloadableImages(const xml_node layout, const std::string& tagName, Page* page) {
	xml_attribute layoutMonitorXml = layout.attribute("monitor");
	int layoutMonitor = layoutMonitorXml.as_int(monitor_); // Fallback to monitor_ if not in layout

	for (xml_node componentXml = layout.child(tagName.c_str()); componentXml; componentXml = componentXml.next_sibling(tagName.c_str())) {
		// Check for monitor attribute on the component, then fall back to layoutMonitor
		xml_attribute monitorXml = componentXml.attribute("monitor");
		int cMonitor = monitorXml.as_int(layoutMonitor);

		xml_attribute type = componentXml.attribute("type");
		xml_attribute imageType = componentXml.attribute("imageType");
		xml_attribute mode = componentXml.attribute("mode");
		xml_attribute timeFormatXml = componentXml.attribute("timeFormat");
		xml_attribute textFormatXml = componentXml.attribute("textFormat");
		xml_attribute singlePrefixXml = componentXml.attribute("singlePrefix");
		xml_attribute singlePostfixXml = componentXml.attribute("singlePostfix");
		xml_attribute pluralPrefixXml = componentXml.attribute("pluralPrefix");
		xml_attribute pluralPostfixXml = componentXml.attribute("pluralPostfix");
		xml_attribute selectedOffsetXml = componentXml.attribute("selectedOffset");
		xml_attribute directionXml = componentXml.attribute("direction");
		xml_attribute scrollingSpeedXml = componentXml.attribute("scrollingSpeed");
		xml_attribute startPositionXml = componentXml.attribute("startPosition");
		xml_attribute startTimeXml = componentXml.attribute("startTime");
		xml_attribute endTimeXml = componentXml.attribute("endTime");
		xml_attribute alignmentXml = componentXml.attribute("alignment");
		xml_attribute idXml = componentXml.attribute("id");
		xml_attribute randomSelectXml = componentXml.attribute("randomSelect");
		xml_attribute locationXml = componentXml.attribute("location");
		xml_attribute baseColumnPaddingXml = componentXml.attribute("baseColumnPadding");
		xml_attribute baseRowPaddingXml = componentXml.attribute("baseRowPadding");
		xml_attribute maxRowsXml = componentXml.attribute("maxRows");
		xml_attribute excludedColumnsXml = componentXml.attribute("excludedColumns");

		bool systemMode = false;
		bool layoutMode = false;
		bool commonMode = false;
		bool menuMode = false;
		int selectedOffset = selectedOffsetXml.as_int();
		int id = idXml.as_int(-1);

		// Image type validation
		if (!imageType && (tagName == "reloadableVideo" || tagName == "reloadableAudio")) {
			LOG_WARNING("Layout", "<reloadableImage> component in layout does not specify an imageType for when the video does not exist");
		}
		if (!type && (tagName == "reloadableImage" || tagName == "reloadableText")) {
			LOG_ERROR("Layout", "Image component in layout does not specify a source image file");
		}
		if (!type && tagName == "reloadableScrollingText") {
			LOG_ERROR("Layout", "Reloadable scrolling text component in layout does not specify a type");
		}

		// Mode handling
		if (mode) {
			std::string sysMode = mode.value();
			systemMode = sysMode == "system" || sysMode == "systemlayout";
			layoutMode = sysMode == "layout" || sysMode == "commonlayout" || sysMode == "systemlayout";
			commonMode = sysMode == "common" || sysMode == "commonlayout";
			menuMode = sysMode == "menu";
		}

		Component* c = nullptr;

		if (tagName == "reloadableText") {
			if (type) {
				FontManager* font = addFont(componentXml, pugi::xml_node{}, cMonitor);
				std::string typeValue = type.value();
				std::string textFormat = textFormatXml ? textFormatXml.value() : "";

				if (typeValue == "file") {
					if (!locationXml) {
						LOG_ERROR("Layout", "reloadableText type='file' requires a 'location' attribute.");
						continue; // Skip this component if location is not provided
					}
					std::string location = locationXml.value();
					c = new ReloadableText(typeValue, *page, config_, systemMode, font, layoutKey, "", "", "", "", "", "", location);
				}
				else {
					std::string singlePrefix = singlePrefixXml ? singlePrefixXml.value() : "";
					std::string singlePostfix = singlePostfixXml ? singlePostfixXml.value() : "";
					std::string pluralPrefix = pluralPrefixXml ? pluralPrefixXml.value() : "";
					std::string pluralPostfix = pluralPostfixXml ? pluralPostfixXml.value() : "";
					std::string timeFormat = timeFormatXml ? timeFormatXml.value() : "";

					c = new ReloadableText(typeValue, *page, config_, systemMode, font, layoutKey, timeFormat, textFormat, singlePrefix, singlePostfix, pluralPrefix, pluralPostfix);
				}
			}
		}
		else if (tagName == "reloadableScrollingText") {
			if (type) {
				FontManager* font = addFont(componentXml, pugi::xml_node{}, cMonitor);
				std::string typeValue = type.value();
				std::string location = (typeValue == "file" && locationXml) ? locationXml.value() : "";
				if (typeValue == "file" && location.empty()) {
					LOG_ERROR("Layout", "reloadableScrollingText type='file' requires a 'location' attribute.");
					continue; // Skip this component if location is not provided
				}

				std::string textFormat = textFormatXml ? textFormatXml.value() : "";
				std::string direction = directionXml ? directionXml.value() : "horizontal";
			float scrollingSpeed = scrollingSpeedXml.as_float(1.0f);
			float startPosition = startPositionXml.as_float();
			float startTime = startTimeXml.as_float();
			float endTime = endTimeXml.as_float();
				std::string alignment = alignmentXml ? alignmentXml.value() : "";
				std::string singlePrefix = singlePrefixXml ? singlePrefixXml.value() : "";
				std::string singlePostfix = singlePostfixXml ? singlePostfixXml.value() : "";
				std::string pluralPrefix = pluralPrefixXml ? pluralPrefixXml.value() : "";
				std::string pluralPostfix = pluralPostfixXml ? pluralPostfixXml.value() : "";

				c = new ReloadableScrollingText(config_, systemMode, layoutMode, menuMode, typeValue,
					textFormat, singlePrefix, singlePostfix, pluralPrefix,
					pluralPostfix, alignment, *page, selectedOffset,
					font, direction, scrollingSpeed, startPosition,
					startTime, endTime, location);
			}
		}
		else if (tagName == "reloadableHiscores") {
			FontManager* font = addFont(componentXml, pugi::xml_node{}, cMonitor);
			std::string textFormat = textFormatXml ? textFormatXml.value() : "";
			float scrollingSpeed = scrollingSpeedXml.as_float(1.0f);
			float startTime = startTimeXml.as_float();
			float baseColumnPadding = baseColumnPaddingXml.as_float(1.5f);
			float baseRowPadding = baseRowPaddingXml.as_float(0.5f);
			size_t maxRows = maxRowsXml ? static_cast<size_t>(maxRowsXml.as_int()) : std::numeric_limits<size_t>::max(); // Default to unlimited rows
			std::string excludedColumns = excludedColumnsXml ? excludedColumnsXml.value() : "";

			c = new ReloadableHiscores(config_, textFormat, *page, selectedOffset,
				font, scrollingSpeed, startTime,
				excludedColumns, baseColumnPadding, baseRowPadding, maxRows);
		}
		else if (tagName == "reloadableGlobalHiscores") {
			FontManager* font = addFont(componentXml, pugi::xml_node{}, cMonitor);
			std::string textFormat = textFormatXml ? textFormatXml.value() : "";
			float baseColumnPadding = baseColumnPaddingXml.as_float(1.5f);
			float baseRowPadding = baseRowPaddingXml.as_float(0.5f);
			std::string excludedColumns = excludedColumnsXml ? excludedColumnsXml.value() : "";

			c = new ReloadableGlobalHiscores(config_, textFormat, *page, selectedOffset,
				font, baseColumnPadding, baseRowPadding);
		}

		else if (tagName == "musicPlayer") {
			std::string typeValue = type.value();

			// Create font for text-based music player components
			FontManager* font = addFont(componentXml, pugi::xml_node{}, cMonitor);

			// Create MusicPlayerComponent with common mode enabled
			c = new MusicPlayerComponent(config_, true, typeValue, *page, cMonitor, font);
		}

		else {
			xml_attribute jukeboxXml = componentXml.attribute("jukebox");
			bool jukebox = jukeboxXml.as_bool();
			int jukeboxNumLoops = componentXml.attribute("jukeboxNumLoops").as_int(1);
			if (jukebox) page->setJukebox();

			FontManager* font = addFont(componentXml, pugi::xml_node{}, cMonitor);
			std::string typeString = type ? type.value() : "video";
			std::string imageTypeString = imageType ? imageType.value() : "";
			int randomSelectInt = randomSelectXml.as_int();

			c = new ReloadableMedia(config_, systemMode, layoutMode, commonMode, menuMode, typeString, imageTypeString, *page, selectedOffset, (tagName == "reloadableVideo") || (tagName == "reloadableAudio"), font, jukebox, jukeboxNumLoops, randomSelectInt);
			if (c) {
				c->allocateGraphicsMemory();
				xml_attribute textFallback = componentXml.attribute("textFallback");
				static_cast<ReloadableMedia*>(c)->enableTextFallback_(textFallback.as_bool());

				xml_attribute useTextureCacheXml = componentXml.attribute("useTextureCache");
				if (useTextureCacheXml.as_bool()) {
					static_cast<ReloadableMedia*>(c)->enableTextureCache_(true);
				}
			}
		}

		// Common setup for all components
		if (c) {
			c->baseViewInfo.Monitor = cMonitor;
			c->baseViewInfo.Layout = page->getCurrentLayout();
			c->setId(id);

			// Set menuScrollReload if applicable
			if (componentXml.attribute("menuScrollReload").as_bool())
			{
				c->setMenuScrollReload(true);
			}

			loadTweens(c, componentXml);
			page->addComponent(c);
		}
	}
}

FontManager* PageBuilder::addFont(const xml_node component, const xml_node defaults, int monitor) {
	xml_attribute fontXml = component.attribute("font");
	xml_attribute fontColorXml = component.attribute("fontColor");
	xml_attribute fontSizeXml = component.attribute("loadFontSize");
	xml_attribute fontGradientXml = component.attribute("fontGradient");
	xml_attribute fontOutlineXml = component.attribute("fontOutline");

	if (defaults) {
		if (!fontXml && defaults.attribute("font")) {
			fontXml = defaults.attribute("font");
		}

		if (!fontColorXml && defaults.attribute("fontColor")) {
			fontColorXml = defaults.attribute("fontColor");
		}

		if (!fontSizeXml && defaults.attribute("loadFontSize")) {
			fontSizeXml = defaults.attribute("loadFontSize");
		}

		if(!fontGradientXml && defaults.attribute("fontGradient")) {
			fontGradientXml = defaults.attribute("fontGradient");
		}
		if (!fontOutlineXml && defaults.attribute("fontOutline")) {
			fontOutlineXml = defaults.attribute("fontOutline");
		}
	}


	// use layout defaults unless overridden
	std::string fontName = fontName_;
	SDL_Color fontColor = fontColor_;
	int fontSize = fontSize_;
	bool fontGradient = fontGradient_;
	int fontOutline = fontOutline_;


	if (fontXml) {
		fontName = Configuration::convertToAbsolutePath(
			Utils::combinePath(Configuration::absolutePath, "layouts", layoutKey, ""),
			fontXml.value());

		LOG_DEBUG("Layout", "loading font " + fontName);
	}
	if (fontColorXml) {
		int intColor = 0;
		std::stringstream ss;
		ss << std::hex << fontColorXml.value();
		ss >> intColor;

		fontColor.b = intColor & 0xFF;
		intColor >>= 8;
		fontColor.g = intColor & 0xFF;
		intColor >>= 8;
		fontColor.r = intColor & 0xFF;
	}

	if (fontSizeXml) {
		fontSize = fontSizeXml.as_int();
	}

	if (fontGradientXml) {
		fontGradient = fontGradientXml.as_bool();
	}

	if (fontOutlineXml) {
		fontOutline = fontOutlineXml.as_int();
	}

	fontCache_->loadFont(fontName, fontSize, fontColor, fontGradient, fontOutline, monitor);
	return fontCache_->getFont(fontName, fontSize, fontColor, fontGradient, fontOutline, monitor);
}

void PageBuilder::loadTweens(Component* c, pugi::xml_node componentXml) {
	buildViewInfo(componentXml, c->baseViewInfo);
	c->setTweens(createTweenInstance(componentXml));
}

std::shared_ptr<AnimationEvents> PageBuilder::createTweenInstance(pugi::xml_node componentXml) {
	auto tweens = std::make_shared<AnimationEvents>();

	// Mapping XML tags to internal event names
	buildTweenSet(tweens.get(), componentXml, "onEnter", "enter");
	buildTweenSet(tweens.get(), componentXml, "onExit", "exit");
	buildTweenSet(tweens.get(), componentXml, "onIdle", "idle");
	buildTweenSet(tweens.get(), componentXml, "onMenuIdle", "menuIdle");
	buildTweenSet(tweens.get(), componentXml, "onMenuScroll", "menuScroll");
	buildTweenSet(tweens.get(), componentXml, "onPlaylistScroll", "playlistScroll");
	buildTweenSet(tweens.get(), componentXml, "onHighlightEnter", "highlightEnter");
	buildTweenSet(tweens.get(), componentXml, "onHighlightExit", "highlightExit");
	buildTweenSet(tweens.get(), componentXml, "onMenuEnter", "menuEnter");
	buildTweenSet(tweens.get(), componentXml, "onMenuExit", "menuExit");
	buildTweenSet(tweens.get(), componentXml, "onGameEnter", "gameEnter");
	buildTweenSet(tweens.get(), componentXml, "onGameExit", "gameExit");
	buildTweenSet(tweens.get(), componentXml, "onPlaylistEnter", "playlistEnter");
	buildTweenSet(tweens.get(), componentXml, "onPlaylistExit", "playlistExit");
	buildTweenSet(tweens.get(), componentXml, "onPlaylistNextEnter", "playlistNextEnter");
	buildTweenSet(tweens.get(), componentXml, "onPlaylistNextExit", "playlistNextExit");
	buildTweenSet(tweens.get(), componentXml, "onPlaylistPrevEnter", "playlistPrevEnter");
	buildTweenSet(tweens.get(), componentXml, "onPlaylistPrevExit", "playlistPrevExit");
	buildTweenSet(tweens.get(), componentXml, "onMenuJumpEnter", "menuJumpEnter");
	buildTweenSet(tweens.get(), componentXml, "onMenuJumpExit", "menuJumpExit");
	buildTweenSet(tweens.get(), componentXml, "onAttractEnter", "attractEnter");
	buildTweenSet(tweens.get(), componentXml, "onAttract", "attract");
	buildTweenSet(tweens.get(), componentXml, "onAttractExit", "attractExit");
	buildTweenSet(tweens.get(), componentXml, "onJukeboxJump", "jukeboxJump");
	buildTweenSet(tweens.get(), componentXml, "onGameInfoEnter", "gameInfoEnter");
	buildTweenSet(tweens.get(), componentXml, "onGameInfoExit", "gameInfoExit");
	buildTweenSet(tweens.get(), componentXml, "onCollectionInfoEnter", "collectionInfoEnter");
	buildTweenSet(tweens.get(), componentXml, "onCollectionInfoExit", "collectionInfoExit");
	buildTweenSet(tweens.get(), componentXml, "onBuildInfoEnter", "buildInfoEnter");
	buildTweenSet(tweens.get(), componentXml, "onBuildInfoExit", "buildInfoExit");
	buildTweenSet(tweens.get(), componentXml, "onMenuActionInputEnter", "menuActionInputEnter");
	buildTweenSet(tweens.get(), componentXml, "onMenuActionInputExit", "menuActionInputExit");
	buildTweenSet(tweens.get(), componentXml, "onMenuActionSelectEnter", "menuActionSelectEnter");
	buildTweenSet(tweens.get(), componentXml, "onMenuActionSelectExit", "menuActionSelectExit");
	buildTweenSet(tweens.get(), componentXml, "onTrackChange", "trackChange");

	return tweens;
}


void PageBuilder::buildTweenSet(AnimationEvents* tweens, pugi::xml_node componentXml, const std::string& tagName, const std::string& tweenName) {
	for (pugi::xml_node tweenNode : componentXml.children(tagName.c_str())) {
		pugi::xml_attribute indexAttr = tweenNode.attribute("menuIndex");

		if (indexAttr) {
			std::string indexs = indexAttr.value();
			if (indexs.empty()) continue;

			if (indexs[0] == '!') {
				indexs.erase(0, 1);
				int index = Utils::convertInt(indexs);
				for (int i = 0; i < MENU_INDEX_HIGH - 1; i++) {
					if (i != index) {
						auto animation = std::make_shared<Animation>();
						getTweenSet(tweenNode, animation.get());
						tweens->setAnimation(tweenName, i, std::move(animation));
					}
				}
			}
			else if (indexs[0] == '<') {
				indexs.erase(0, 1);
				int index = Utils::convertInt(indexs);
				for (int i = 0; i < MENU_INDEX_HIGH - 1; i++) {
					if (i < index) {
						auto animation = std::make_shared<Animation>();
						getTweenSet(tweenNode, animation.get());
						tweens->setAnimation(tweenName, i, std::move(animation));
					}
				}
			}
			else if (indexs[0] == '>') {
				indexs.erase(0, 1);
				int index = Utils::convertInt(indexs);
				for (int i = 0; i < MENU_INDEX_HIGH - 1; i++) {
					if (i > index) {
						auto animation = std::make_shared<Animation>();
						getTweenSet(tweenNode, animation.get());
						tweens->setAnimation(tweenName, i, std::move(animation));
					}
				}
			}
			else if (indexs[0] == 'i') {
				auto animation = std::make_shared<Animation>();
				getTweenSet(tweenNode, animation.get());
				tweens->setAnimation(tweenName, MENU_INDEX_HIGH, std::move(animation));
			}
			else {
				int index = indexAttr.as_int();
				auto animation = std::make_shared<Animation>();
				getTweenSet(tweenNode, animation.get());
				tweens->setAnimation(tweenName, index, std::move(animation));
			}
		}
		else {
			auto animation = std::make_shared<Animation>();
			getTweenSet(tweenNode, animation.get());
			tweens->setAnimation(tweenName, -1, std::move(animation));
		}
	}
}


ScrollingList* PageBuilder::buildMenu(xml_node menuXml, Page& page, int monitor) {
	ScrollingList* menu = nullptr;
	std::string menuType = "vertical";
	std::string imageType = "null";
	std::string videoType = "null";
	std::map<int, xml_node> overrideItems;
	xml_node itemDefaults = menuXml.child("itemDefaults");
	xml_attribute modeXml = menuXml.attribute("mode");
	xml_attribute imageTypeXml = menuXml.attribute("imageType");
	xml_attribute videoTypeXml = menuXml.attribute("videoType");
	xml_attribute menuTypeXml = menuXml.attribute("type");
	xml_attribute scrollTimeXml = menuXml.attribute("scrollTime");
	xml_attribute scrollAccelerationXml = menuXml.attribute("scrollAcceleration");
	xml_attribute minScrollTimeXml = menuXml.attribute("minScrollTime");
	xml_attribute scrollOrientationXml = menuXml.attribute("orientation");
	xml_attribute selectedImage = menuXml.attribute("selectedImage");
	xml_attribute textFallbackXml = menuXml.attribute("textFallback");
	xml_attribute monitorXml = menuXml.attribute("monitor");
	xml_attribute useTextureCacheXml = menuXml.attribute("useTextureCache");
	xml_attribute perspectiveXml = menuXml.attribute("usePerspective");
	xml_attribute topLeftXml = menuXml.attribute("topLeft");
	xml_attribute topRightXml = menuXml.attribute("topRight");
	xml_attribute bottomLeftXml = menuXml.attribute("bottomLeft");
	xml_attribute bottomRightXml = menuXml.attribute("bottomRight");

	if (menuTypeXml) {
		menuType = menuTypeXml.value();
	}

	// ensure <menu> has an <itemDefaults> tag
	if (!itemDefaults) {
		LOG_WARNING("Layout", "Menu tag is missing <itemDefaults> tag.");
	}

	bool playlistType = false;
	if (imageTypeXml) {
		imageType = imageTypeXml.value();
		if (imageType.rfind("playlist", 0) == 0) {
			playlistType = true;
		}
	}

	if (videoTypeXml) {
		videoType = videoTypeXml.value();
		if (videoType.rfind("playlist", 0) == 0) {
			playlistType = true;
		}
	}

	bool layoutMode = false;
	bool commonMode = false;
	if (modeXml) {
		std::string sysMode = modeXml.value();
		if (sysMode == "layout") {
			layoutMode = true;
		}
		if (sysMode == "common") {
			commonMode = true;
		}
		if (sysMode == "commonlayout") {
			layoutMode = true;
			commonMode = true;
		}
	}

	int cMonitor = monitorXml.as_int(monitor);

	// on default, text will be rendered to the menu. Preload it into cache.
	FontManager* font = addFont(itemDefaults, pugi::xml_node{}, cMonitor);

	bool useTextureCache = false;
	if (useTextureCacheXml.as_bool()) {
		useTextureCache = true;
	}

	menu = new ScrollingList(config_, page, layoutMode, commonMode, playlistType, selectedImage, font, layoutKey, imageType, videoType, useTextureCache);
	menu->baseViewInfo.Monitor = cMonitor;
	menu->baseViewInfo.Layout = page.getCurrentLayout();

	if (videoType != "null" && perspectiveXml) {
		bool usePerspective = perspectiveXml.as_bool();

		// In your PageBuilder.cpp, instead of using Utils::split, use Utils::listToVector
		if (usePerspective) {
			// Only process corner coordinates if perspective is enabled
			if (topLeftXml && topRightXml && bottomLeftXml && bottomRightXml) {
				std::vector<std::string> topLeft;
				std::vector<std::string> topRight;
				std::vector<std::string> bottomLeft;
				std::vector<std::string> bottomRight;

				Utils::listToVector(topLeftXml.value(), topLeft, ',');
				Utils::listToVector(topRightXml.value(), topRight, ',');
				Utils::listToVector(bottomLeftXml.value(), bottomLeft, ',');
				Utils::listToVector(bottomRightXml.value(), bottomRight, ',');

				if (topLeft.size() == 2 && topRight.size() == 2 &&
					bottomLeft.size() == 2 && bottomRight.size() == 2) {

					int corners[8] = {
						Utils::convertInt(topLeft[0]),     // top left x
						Utils::convertInt(topLeft[1]),     // top left y
						Utils::convertInt(topRight[0]),    // top right x
						Utils::convertInt(topRight[1]),    // top right y
						Utils::convertInt(bottomLeft[0]),  // bottom left x
						Utils::convertInt(bottomLeft[1]),  // bottom left y
						Utils::convertInt(bottomRight[0]), // bottom right x
						Utils::convertInt(bottomRight[1])  // bottom right y
					};
					menu->setPerspectiveCorners(corners);
				}
				else {
					LOG_WARNING("Layout", "Invalid coordinate format for perspective corners. Expected 'x,y'");
				}
			}
		}
	}

	buildViewInfo(menuXml, menu->baseViewInfo);

	if (scrollTimeXml) {
		menu->setStartScrollTime(scrollTimeXml.as_float());
	}

	if (scrollAccelerationXml) {
		menu->setScrollAcceleration(scrollAccelerationXml.as_float());
		menu->setMinScrollTime(scrollAccelerationXml.as_float());
	}

	if (minScrollTimeXml) {
		menu->setMinScrollTime(minScrollTimeXml.as_float());
	}

	if (scrollOrientationXml) {
		std::string scrollOrientation = scrollOrientationXml.value();
		if (scrollOrientation == "horizontal") {
			menu->horizontalScroll = true;
		}
	}

	if (textFallbackXml.as_bool()) {
		menu->enableTextFallback(true);
	}

	buildViewInfo(menuXml, menu->baseViewInfo);

	if (menuType == "custom") {
		buildCustomMenu(menu, menuXml, itemDefaults);
	}
	else {
		buildVerticalMenu(menu, menuXml, itemDefaults);
	}

	loadTweens(menu, menuXml);

	return menu;
}


void PageBuilder::buildCustomMenu(ScrollingList* menu, const pugi::xml_node menuXml, pugi::xml_node itemDefaults) {
	auto points = new std::vector<ViewInfo*>(); // Leave ViewInfo unchanged
	auto tweenPoints = std::make_shared<std::vector<std::shared_ptr<AnimationEvents>>>();

	int i = 0;

	for (auto componentXml = menuXml.child("item"); componentXml; componentXml = componentXml.next_sibling("item")) {
		auto* viewInfo = new ViewInfo(); // Leave ViewInfo unchanged
		viewInfo->Monitor = menu->baseViewInfo.Monitor;
		viewInfo->Layout = menu->baseViewInfo.Layout;

		buildViewInfo(componentXml, *viewInfo, itemDefaults);
		viewInfo->Additive = menu->baseViewInfo.Additive;

		points->push_back(viewInfo);
		tweenPoints->push_back(createTweenInstance(componentXml));

		if (componentXml.attribute("selected")) {
			menu->setSelectedIndex(i);
		}

		i++;
	}

	menu->setPoints(points, tweenPoints);
}

void PageBuilder::buildVerticalMenu(ScrollingList* menu, const pugi::xml_node menuXml, pugi::xml_node itemDefaults) {
	auto points = new std::vector<ViewInfo*>(); // Leave ViewInfo unchanged
	auto tweenPoints = std::make_shared<std::vector<std::shared_ptr<AnimationEvents>>>();

	int selectedIndex = MENU_FIRST;
	std::map<int, pugi::xml_node > overrideItems;

	// By default the menu will automatically determine the offsets for your list items.
	// We can override individual menu points to have unique characteristics (i.e. make the first item opaque or
	// make the selected item a different color).
	for (auto componentXml = menuXml.child("item"); componentXml; componentXml = componentXml.next_sibling("item")) {
		const auto xmlIndex = componentXml.attribute("index");

		if (xmlIndex) {
			int itemIndex = parseMenuPosition(xmlIndex.value());
			overrideItems[itemIndex] = componentXml;

			// check to see if the item specified is the selected index
			const auto xmlSelectedIndex = componentXml.attribute("selected");

			if (xmlSelectedIndex) {
				selectedIndex = itemIndex;
			}
		}
	}

	bool end = false;

	// menu start

	float height = 0;
	int index = 0;

	if (overrideItems.find(MENU_START) != overrideItems.end()) {
		auto component = overrideItems[MENU_START];
		auto* viewInfo = createMenuItemInfo(component, itemDefaults, menu->baseViewInfo);
		viewInfo->Y = menu->baseViewInfo.Y + height;
		points->push_back(viewInfo);
		tweenPoints->push_back(createTweenInstance(component));
		height += viewInfo->Height;

		// increment the selected index to account for the new "invisible" menu item
		selectedIndex++;
	}
	while (!end) {
		auto* viewInfo = new ViewInfo();
		viewInfo->Monitor = menu->baseViewInfo.Monitor;
		viewInfo->Layout = menu->baseViewInfo.Layout;

		auto component = itemDefaults;

		// use overridden item setting if specified by layout for the given index
		if (overrideItems.find(index) != overrideItems.end()) {
			component = overrideItems[index];
		}

		// calculate the total height of our menu items if we can load any additional items
		buildViewInfo(component, *viewInfo, itemDefaults);
		const auto itemSpacingXml = component.attribute("spacing");
		int itemSpacing = itemSpacingXml.as_int();
		float nextHeight = height + viewInfo->Height + itemSpacing;

		if (nextHeight >= menu->baseViewInfo.Height) {
			end = true;
		}

		// we have reached the last menu item
		if (end && overrideItems.find(MENU_LAST) != overrideItems.end()) {
			component = overrideItems[MENU_LAST];

			buildViewInfo(component, *viewInfo, itemDefaults);
			const auto itemSpacingXml = component.attribute("spacing");
			int itemSpacing = itemSpacingXml.as_int();
			nextHeight = height + viewInfo->Height + itemSpacing;
		}

		viewInfo->Y = menu->baseViewInfo.Y + height;
		points->push_back(viewInfo);
		tweenPoints->push_back(createTweenInstance(component));
		index++;
		height = nextHeight;
	}

	// menu end
	if (overrideItems.find(MENU_END) != overrideItems.end()) {
		auto component = overrideItems[MENU_END];
		auto* viewInfo = createMenuItemInfo(component, itemDefaults, menu->baseViewInfo);
		viewInfo->Y = menu->baseViewInfo.Y + height;
		points->push_back(viewInfo);
		tweenPoints->push_back(createTweenInstance(component));
	}

	if (selectedIndex >= static_cast<int>(points->size())) {
		std::stringstream ss;
		ss << "Design error! Selected menu item was set to " << selectedIndex
			<< " although there are only " << points->size()
			<< " menu points that can be displayed";

		LOG_ERROR("Layout", ss.str());
		selectedIndex = 0;
	}

	menu->setSelectedIndex(selectedIndex);
	menu->setPoints(points, std::move(tweenPoints)); // Use std::move to transfer ownership of the shared pointer
}

ViewInfo* PageBuilder::createMenuItemInfo(xml_node component, xml_node defaults, const ViewInfo& menuViewInfo) {
	auto* viewInfo = new ViewInfo();
	viewInfo->Monitor = menuViewInfo.Monitor;

	buildViewInfo(component, *viewInfo, defaults);

	return viewInfo;
}

int PageBuilder::parseMenuPosition(const std::string& strIndex) {
	int index = MENU_FIRST;

	if (strIndex == "end") {
		index = MENU_END;
	}
	else if (strIndex == "last") {
		index = MENU_LAST;
	}
	else if (strIndex == "start") {
		index = MENU_START;
	}
	else if (strIndex == "first") {
		index = MENU_FIRST;
	}
	else {
		index = Utils::convertInt(strIndex);
	}
	return index;
}

xml_attribute PageBuilder::findAttribute(const xml_node componentXml, const std::string& attribute, const xml_node defaultXml) {
	xml_attribute attributeXml = componentXml.attribute(attribute.c_str());

	if (!attributeXml && defaultXml) {
		attributeXml = defaultXml.attribute(attribute.c_str());
	}

	return attributeXml;
}

void PageBuilder::buildViewInfo(xml_node componentXml, ViewInfo& info, xml_node defaultXml) {
	xml_attribute x = findAttribute(componentXml, "x", defaultXml);
	xml_attribute y = findAttribute(componentXml, "y", defaultXml);
	xml_attribute xOffset = findAttribute(componentXml, "xOffset", defaultXml);
	xml_attribute yOffset = findAttribute(componentXml, "yOffset", defaultXml);
	xml_attribute xOrigin = findAttribute(componentXml, "xOrigin", defaultXml);
	xml_attribute yOrigin = findAttribute(componentXml, "yOrigin", defaultXml);
	xml_attribute height = findAttribute(componentXml, "height", defaultXml);
	xml_attribute width = findAttribute(componentXml, "width", defaultXml);
	xml_attribute fontSize = findAttribute(componentXml, "fontSize", defaultXml);
	xml_attribute fontColor = findAttribute(componentXml, "fontColor", defaultXml);
	xml_attribute minHeight = findAttribute(componentXml, "minHeight", defaultXml);
	xml_attribute minWidth = findAttribute(componentXml, "minWidth", defaultXml);
	xml_attribute maxHeight = findAttribute(componentXml, "maxHeight", defaultXml);
	xml_attribute maxWidth = findAttribute(componentXml, "maxWidth", defaultXml);
	xml_attribute alpha = findAttribute(componentXml, "alpha", defaultXml);
	xml_attribute angle = findAttribute(componentXml, "angle", defaultXml);
	xml_attribute layer = findAttribute(componentXml, "layer", defaultXml);
	xml_attribute backgroundColor = findAttribute(componentXml, "backgroundColor", defaultXml);
	xml_attribute backgroundAlpha = findAttribute(componentXml, "backgroundAlpha", defaultXml);
	xml_attribute reflection = findAttribute(componentXml, "reflection", defaultXml);
	xml_attribute reflectionDistance = findAttribute(componentXml, "reflectionDistance", defaultXml);
	xml_attribute reflectionScale = findAttribute(componentXml, "reflectionScale", defaultXml);
	xml_attribute reflectionAlpha = findAttribute(componentXml, "reflectionAlpha", defaultXml);
	xml_attribute containerX = findAttribute(componentXml, "containerX", defaultXml);
	xml_attribute containerY = findAttribute(componentXml, "containerY", defaultXml);
	xml_attribute containerWidth = findAttribute(componentXml, "containerWidth", defaultXml);
	xml_attribute containerHeight = findAttribute(componentXml, "containerHeight", defaultXml);
	xml_attribute monitor = findAttribute(componentXml, "monitor", defaultXml);
	xml_attribute volume = findAttribute(componentXml, "volume", defaultXml);
	xml_attribute restart = findAttribute(componentXml, "restart", defaultXml);
	xml_attribute additive = findAttribute(componentXml, "additive", defaultXml);
	xml_attribute pauseOnScroll = findAttribute(componentXml, "pauseOnScroll", defaultXml);

	info.X = getHorizontalAlignment(x, 0);
	info.Y = getVerticalAlignment(y, 0);

	info.XOffset = getHorizontalAlignment(xOffset, 0);
	info.YOffset = getVerticalAlignment(yOffset, 0);
	float xOriginRelative = getHorizontalAlignment(xOrigin, 0);
	float yOriginRelative = getVerticalAlignment(yOrigin, 0);

	// the origins need to be saved as a percent since the heights and widths can be scaled
	info.XOrigin = xOriginRelative / layoutWidth_;
	info.YOrigin = yOriginRelative / layoutHeight_;


	if (!height && !width) {
		info.Height = -1;
		info.Width = -1;
	}
	else {
		info.Height = getVerticalAlignment(height, -1);
		info.Width = getHorizontalAlignment(width, -1);
	}
	info.FontSize = getVerticalAlignment(fontSize, -1);
	info.MinHeight = getVerticalAlignment(minHeight, 0);
	info.MinWidth = getHorizontalAlignment(minWidth, 0);
	info.MaxHeight = getVerticalAlignment(maxHeight, FLT_MAX);
	info.MaxWidth = getVerticalAlignment(maxWidth, FLT_MAX);
	info.Alpha = alpha.as_float(1.f);
	info.Angle = angle.as_float();
	info.Layer = layer.as_int();
	info.Reflection = reflection ? reflection.value() : "";
	// Parse reflection string
	info.reflectionMask = 0;
	if (!info.Reflection.empty()) {
		std::istringstream ss(info.Reflection);
		std::string token;
		while (ss >> token) {
			token = Utils::toLower(token);
			if (token == "top")
				info.reflectionMask |= 1u << 0;
			else if (token == "bottom")
				info.reflectionMask |= 1u << 1;
			else if (token == "left")
				info.reflectionMask |= 1u << 2;
			else if (token == "right")
				info.reflectionMask |= 1u << 3;
			else
				LOG_WARNING("PageBuilder", "Unknown reflection token: " + token + " (valid: top, bottom, left, right)");
		}
	}
	// Precompute flag for draw
	info.hasReflection = (info.reflectionMask != 0) && (info.ReflectionAlpha > 0.f) && (info.ReflectionScale > 0.f);
	info.ReflectionDistance = reflectionDistance.as_int();
	info.ReflectionScale = reflectionScale.as_float(0.25f);
	info.ReflectionAlpha = reflectionAlpha.as_float(1.f);
	info.ContainerX = containerX.as_float();
	info.ContainerY = containerY.as_float();
	info.ContainerWidth = containerWidth.as_float(-1.f);
	info.ContainerHeight = containerHeight.as_float(-1.f);
	info.Monitor = monitor.as_int(info.Monitor);
	info.Volume = volume.as_float(1.f);
	info.Restart = restart.as_bool();
	info.Additive = additive.as_bool();

	// If pauseOnScroll is not set, default to true; otherwise use its bool value
	info.PauseOnScroll = pauseOnScroll.as_bool(true);

	// This reads the configuration and sets Restart or PauseOnScroll accordingly
	bool disableVideoRestart = false;
	bool disablePauseOnScroll = false;

	// Check if the property exists and is set to true
	if (config_.getProperty(OPTION_DISABLEVIDEORESTART, disableVideoRestart) && disableVideoRestart) {
		info.Restart = false;
	}

	// Check if the property exists and is set to true
	if (config_.getProperty(OPTION_DISABLEPAUSEONSCROLL, disablePauseOnScroll) && disablePauseOnScroll) {
		info.PauseOnScroll = false;
	}

	if (fontColor) {
		FontManager* font = addFont(componentXml, defaultXml, info.Monitor);
		info.font = font;
	}

	if (backgroundColor) {
		std::stringstream ss(backgroundColor.value());
		int num;
		ss >> std::hex >> num;
		int red = num / 0x10000;
		int green = (num / 0x100) % 0x100;
		int blue = num % 0x100;

		info.BackgroundRed = static_cast<float>(red / 255);
		info.BackgroundGreen = static_cast<float>(green / 255);
		info.BackgroundBlue = static_cast<float>(blue / 255);
	}

	if (backgroundAlpha) {
		info.BackgroundAlpha = backgroundAlpha.as_float(1.f);
	}
}

void PageBuilder::getTweenSet(pugi::xml_node node, Animation* animation) {
	if (node) {
		for (pugi::xml_node set : node.children("set")) {
			auto ts = std::make_shared<TweenSet>();
			getAnimationEvents(set, *ts);
			animation->Push(ts);
		}
	}
}

void PageBuilder::getAnimationEvents(pugi::xml_node node, TweenSet& tweens) {
	pugi::xml_attribute durationAttr = node.attribute("duration");
	if (!durationAttr) {
		LOG_ERROR("Layout", "Animation set tag missing \"duration\" attribute");
		return;
	}

	std::string actionSetting;
	config_.getProperty(OPTION_ACTION, actionSetting);

	for (pugi::xml_node animate : node.children("animate")) {
		pugi::xml_attribute typeAttr = animate.attribute("type");
		pugi::xml_attribute fromAttr = animate.attribute("from");
		pugi::xml_attribute toAttr = animate.attribute("to");

		if (!typeAttr) {
			LOG_ERROR("Layout", "Animate tag missing \"type\" attribute");
			continue;
		}

		std::string animateType = typeAttr.value();

		if (!toAttr && animateType != "nop" && animateType != "restart") {
			LOG_ERROR("Layout", "Animate tag missing \"to\" attribute");
			continue;
		}

		// Action filter
		if (auto settingAttr = animate.attribute("setting")) {
			if (std::string(settingAttr.value()) != actionSetting) continue;
		}

		float fromValue = 0.0f;
		bool fromDefined = (fromAttr != nullptr);

		// Helper for raw animation values (standard string or percent)
		auto parseAnimateValue = [&](pugi::xml_attribute attr, bool isHorizontal) -> float {
			if (!attr) return 0.0f;
			std::string s = attr.value();
			if (s == "left" || s == "top") return 0.0f;
			if (s == "center") return isHorizontal ? (float)layoutWidth_ / 2.0f : (float)layoutHeight_ / 2.0f;
			if (s == "right" || s == "stretch") return (float)layoutWidth_;
			if (s == "bottom") return (float)layoutHeight_;

			if (!s.empty() && s.back() == '%') {
				float pct = Utils::convertFloat(s.substr(0, s.length() - 1)) / 100.0f;
				return isHorizontal ? (float)layoutWidth_ * pct : (float)layoutHeight_ * pct;
			}
			return Utils::convertFloat(s);
			};

		// Get property type
		auto optProperty = Tween::getTweenProperty(animateType);
		if (!optProperty) {
			LOG_ERROR("Layout", "Unsupported tween type: " + animateType);
			continue;
		}

		TweenProperty property = *optProperty;
		float toValue = 0.0f;

		// Perform layout scaling based on property type
		switch (property) {
			case TWEEN_PROPERTY_WIDTH:
			case TWEEN_PROPERTY_X:
			case TWEEN_PROPERTY_X_OFFSET:
			case TWEEN_PROPERTY_CONTAINER_X:
			case TWEEN_PROPERTY_CONTAINER_WIDTH:
			fromValue = getHorizontalAlignment(fromAttr, 0);
			toValue = getHorizontalAlignment(toAttr, 0);
			break;

			case TWEEN_PROPERTY_X_ORIGIN:
			fromValue = getHorizontalAlignment(fromAttr, 0) / (float)layoutWidth_;
			toValue = getHorizontalAlignment(toAttr, 0) / (float)layoutWidth_;
			break;

			case TWEEN_PROPERTY_HEIGHT:
			case TWEEN_PROPERTY_Y:
			case TWEEN_PROPERTY_Y_OFFSET:
			case TWEEN_PROPERTY_FONT_SIZE:
			case TWEEN_PROPERTY_CONTAINER_Y:
			case TWEEN_PROPERTY_CONTAINER_HEIGHT:
			fromValue = getVerticalAlignment(fromAttr, 0);
			toValue = getVerticalAlignment(toAttr, 0);
			break;

			case TWEEN_PROPERTY_Y_ORIGIN:
			fromValue = getVerticalAlignment(fromAttr, 0) / (float)layoutHeight_;
			toValue = getVerticalAlignment(toAttr, 0) / (float)layoutHeight_;
			break;

			case TWEEN_PROPERTY_MAX_WIDTH:
			case TWEEN_PROPERTY_MAX_HEIGHT:
			fromValue = getVerticalAlignment(fromAttr, FLT_MAX);
			toValue = getVerticalAlignment(toAttr, FLT_MAX);
			break;

			default:
			// For properties like alpha, rotation, volume: no special alignment logic
			fromValue = fromAttr ? Utils::convertFloat(fromAttr.value()) : 0.0f;
			toValue = toAttr ? Utils::convertFloat(toAttr.value()) : 0.0f;
			break;
		}

		float durationValue = durationAttr.as_float();
		TweenAlgorithm algorithm = LINEAR;
		if (auto algoAttr = animate.attribute("algorithm")) {
			algorithm = Tween::getTweenType(algoAttr.value());
		}

		std::string playlistFilter = animate.attribute("playlist").as_string("");
		auto t = std::make_unique<Tween>(property, algorithm, fromValue, toValue, durationValue, playlistFilter);
		t->startDefined = fromDefined;
		tweens.push(std::move(t));
	}
}