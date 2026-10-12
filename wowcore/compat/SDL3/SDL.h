// WoWBridge compat: the three clipboard calls and one keycode WoWee's Lua engine takes from SDL. The host provides
// the clipboard (Win32 in the plugin, in-memory in tests); keys arrive as SDL-numbered keycodes from WoWBridge input.
#pragma once
#include <cstdint>
extern "C" {
char* SDL_GetClipboardText(void);
bool SDL_SetClipboardText(const char* text);
void SDL_free(void* mem);
}
#define SDLK_DELETE 0x4000004Cu
