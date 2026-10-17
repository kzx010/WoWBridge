#pragma once

#include "wowcore/item_preview.hpp"

// Skyrim's native item-menu draw layer, with an optional WoW item preview. Failure always keeps the native model.
namespace ItemPreview
{
	bool Install();  // optional, verified AE 1.7.104 hooks; false leaves the plugin's other features working
	void Init();     // kDataLoaded: preload catalog assets off the render thread
	void Reset();
	void Redress();  // Avatar::Redress: refresh body items if their race/sex changes

	// Shared immutable CPU assets for menu and ground draws. Display/type/race/sex identify the cache;
	// bounds cover the same Stand t=0 geometry that the menu displays (one standalone shoulder).
	struct Asset
	{
		std::shared_ptr<const wowcore::ItemPreview> item;
		glm::vec3 centre{};
		glm::vec3 size{};
		float radius = 0;
		glm::vec3 longAxis{ 0.0f, 0.0f, 1.0f };
		std::uint32_t inventoryType = 0;
	};
	std::shared_ptr<const Asset> AssetFor(std::uint32_t a_entry);
	void Want(std::uint32_t a_entry);   // an Armory item shown: load its assets in the background (AssetFor has it after)

	struct Info
	{
		bool installed = false, enabled = false, wow = false, armorOnly = false;
		std::uint32_t entry = 0, form = 0, batches = 0;
		std::uint64_t generation = 0, frames = 0, wowFrames = 0, nativeFrames = 0;
		float zoom = 0, rotation[9]{};
		std::string menu, reason;
	};
	Info GetInfo();  // SelfTest: what was actually drawn, copied under a mutex
	std::string ModelFor(std::uint32_t a_entry);   // SelfTest: the loaded item's body or standalone model
}
