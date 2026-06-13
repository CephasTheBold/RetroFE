#pragma once

#include "Utility/SDLCompatibility.h"

class InputHandler
{
public:
    virtual ~InputHandler() = default;;
    virtual bool update(SDL_Event &e) = 0;
    virtual bool pressed() = 0;
    virtual void reset() = 0;
	virtual void updateKeystate() = 0;
    virtual void beginFrame() {}
};
