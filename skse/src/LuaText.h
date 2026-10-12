#pragma once

// A Lua string literal for text going into the WoW interface's Lua (names, messages).
inline std::string LuaQuote(std::string_view a_text)
{
	std::string out = "\"";
	for (const char c : a_text) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		if (static_cast<unsigned char>(c) < 0x20) {
			out += std::format("\\{:03d}", static_cast<int>(static_cast<unsigned char>(c)));
		} else {
			out += c;
		}
	}
	return out + "\"";
}
