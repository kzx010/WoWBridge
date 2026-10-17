#include "Gear.h"
#include "Integration.h"

#include "Armory.h"
#include "Avatar.h"
#include "Cast.h"
#include "Character.h"
#include "Config.h"
#include "Frame.h"
#include "LuaText.h"
#include "ui/WowUi.h"

#include <fstream>

namespace Cast { void UseItem(std::uint32_t entry); }

namespace Gear
{
	namespace
	{
		constexpr std::string_view kPlugin = "WoWBridge.esp";

		struct Item
		{
			std::uint32_t       entry = 0;
			RE::TESBoundObject* form = nullptr;
			RE::SpellItem*      ability = nullptr;
		};
		std::vector<Item>          g_items;
		struct SkyrimLook
		{
			Character::Item item;
			std::vector<Character::Item> additional;
		};
		std::map<RE::TESBoundObject*, SkyrimLook> g_skyrimLooks;
		std::map<std::uint32_t, RE::TESBoundObject*> g_reagents;   // by WoW item entry
		std::map<RE::SpellItem*, std::pair<std::int32_t, std::size_t>> g_armoryCopies;   // the Armory's abilities: (bonus, copy)
		std::map<std::uint32_t, RE::SpellItem*> g_armorySets;   // its set bonuses by local ID
		std::map<std::uint32_t, int> g_shownCounts;                // what the interface was last told
		constexpr int              kReagentStack = 20;
		bool                       g_given = false;
		std::atomic<bool>          g_dirty = false;
		std::vector<Character::Item> g_worn;
		bool                       g_known = false;   // g_worn has been read since the game loaded
		double                     g_nextRead = 0;
		// No load notice yet: a game entered from the main menu by `coc` sends neither kNewGame nor kPostLoadGame, so the
		// first frame the player is in the world counts as one. A save being loaded clears it (kPreLoadGame) and sends its own.
		bool                       g_unannounced = true;

		// One piece to a body slot, as Skyrim keeps it: their WoW items have no armour addons (their Skyrim body is not seen), and
		// Skyrim finds two pieces at odds by their addons - so a Skyrim helmet and their WoW helm were both worn. Whatever the player
		// puts on takes off the worn armour that shares a body slot with it.
		const Item* Find(const RE::TESBoundObject* a_form);

		void OnePerSlot(RE::FormID a_equipped)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			const auto* put = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_equipped);
			if (!player || !put) {
				return;
			}
			// A WoW head item takes the head, the hair and the circlet: one head piece, whichever look it is worn in.
			constexpr std::uint32_t kHeadPieces = (1u << 0) | (1u << 1) | (1u << 12);   // biped slots 30, 31, 42
			const auto claims = [&](const RE::TESObjectARMO* a_armo, std::uint32_t a_type) {
				return a_armo->GetSlotMask().underlying() | (a_type == 1 ? kHeadPieces : 0u);
			};
			// WoW's inventory type of a WoW piece (0: Skyrim's own). Two fingers and two trinkets each keep one other;
			// a shirt and a tabard share Skyrim's body slot 56 but are two WoW slots (INVTYPE_BODY, INVTYPE_TABARD).
			const auto wowType = [](const RE::TESObjectARMO* a_armo) -> std::uint32_t {
				if (const auto* armory = Armory::Find(a_armo)) return armory->look.inventoryType;
				if (const auto* own = Find(a_armo)) return Character::Gear().at(own->entry).inventoryType;
				return 0;
			};
			const auto type = wowType(put);
			const auto slots = claims(put, type);
			const bool dual = type == 11 || type == 12;
			bool kept = false;
			for (const auto& [form, data] : player->GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); })) {
				const auto* other = form->As<RE::TESObjectARMO>();
				if (other && other != put && data.second && data.second->IsWorn() && (claims(other, wowType(other)) & slots)) {
					const auto otherType = wowType(other);
					if ((type == 4 && otherType == 19) || (type == 19 && otherType == 4)) continue;
					if (dual && otherType == type && !kept) {
						kept = true;
						continue;
					}
					RE::ActorEquipManager::GetSingleton()->UnequipObject(player, form, nullptr, 1, nullptr, false, false, false);
				}
			}
		}

		struct EquipSink : RE::BSTEventSink<RE::TESEquipEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (Integration::Active() && a_event && a_event->actor && a_event->actor->IsPlayerRef()) {
					g_dirty = true;
					if (a_event->equipped) {
						SKSE::GetTaskInterface()->AddTask([form = a_event->baseObject] { if (Integration::Active()) OnePerSlot(form); });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_sink;

		const Item* Find(const RE::TESBoundObject* a_form)
		{
			const auto it = std::ranges::find(g_items, a_form, &Item::form);
			return it == g_items.end() ? nullptr : &*it;
		}

		void LoadSkyrimLooks(RE::TESDataHandler* a_data)
		{
			if (!Config::Get().skyrimGearLooks) {
				return;
			}
			const std::filesystem::path path = "Data/SKSE/Plugins/WoWBridge/skyrim_gear_looks.json";
			std::ifstream in(path);
			try {
				const auto catalog = nlohmann::json::parse(in);
				if (catalog.at("schema_version") != 1) {
					throw std::runtime_error("unsupported gear look schema");
				}
				std::map<RE::TESBoundObject*, SkyrimLook> looks;
				std::size_t missing = 0;
				for (const auto& [key, row] : catalog.at("items").items()) {
					// LookupForm resolves both full and light plugins; the catalog stores only the owner's local ID.
					auto* found = a_data->LookupForm(row.at("local_form_id").get<std::uint32_t>(), row.at("plugin").get<std::string>());
					auto* form = found ? found->As<RE::TESBoundObject>() : nullptr;
					if (!form || (!form->As<RE::TESObjectARMO>() && !form->As<RE::TESObjectWEAP>())) {
						++missing;
						continue;
					}
					auto item = [&](const nlohmann::json& r) {
						Character::Item i;
						i.inventoryType = r.at("inventory_type");
						i.slot = Character::SlotOf(i.inventoryType);
						i.displayId = r.at("display_id");
						i.itemClass = r.at("class"), i.subclass = r.at("subclass"), i.sheath = r.at("sheath");
						i.material = r.value("material", 0u);
						i.form = form->GetFormID();
						return i;
					};
					SkyrimLook look{ item(row), {} };
					for (const auto& extra : row.value("additional", nlohmann::json::array())) {
						look.additional.push_back(item(extra));
					}
					looks.emplace(form, std::move(look));
				}
				g_skyrimLooks = std::move(looks);
				logger::info("gear: {} Skyrim looks resolved, {} unavailable forms", g_skyrimLooks.size(), missing);
			} catch (const std::exception& e) {
				logger::error("gear: {}: {}", path.string(), e.what());
			}
		}

		// One display per WoW slot. Skyrim head bits can coexist with WoWBridge's head bit in the inventory;
		// the native helm (or integrated hood) takes that visual slot. Actual legwear wins over coordinated legs.
		std::vector<Character::Item> Equipped()
		{
			std::vector<Character::Item> primary, additional;
			auto* player = RE::PlayerCharacter::GetSingleton();
			for (const auto& [form, data] : player->GetInventory([](RE::TESBoundObject& o) {
				return Find(&o) != nullptr || Armory::Find(&o) != nullptr || g_skyrimLooks.contains(&o);
			})) {
				if (data.first <= 0 || !data.second || !data.second->IsWorn()) {
					continue;
				}
				Character::Item item;
				if (const auto* own = Find(form)) {
					item = Character::Gear().at(own->entry);
					item.form = form->GetFormID();
				} else if (const auto* armory = Armory::Find(form)) {
					item = armory->look;
					item.form = form->GetFormID();
				} else {
					const auto& look = g_skyrimLooks.at(form);
					item = look.item;
					additional.insert(additional.end(), look.additional.begin(), look.additional.end());
				}
				if (form->As<RE::TESObjectWEAP>()) {
					const bool right = player->GetEquippedObject(false) == form;
					const bool left = player->GetEquippedObject(true) == form;
					// Worn inventory flags can outlive the hand assignment during an equip. They do not
					// make a weapon with neither hand assigned into a main-hand weapon.
					if (!right && !left) { continue; }
					if (left && (!right || item.inventoryType == 13 || item.inventoryType == 21 || item.inventoryType == 22)) {
						auto off = item;
						off.slot = 16;
						primary.push_back(off);
						if (!right) { continue; }
					}
					// Skyrim bows/crossbows are held weapons; WoW's ranged inventory slot is not drawn by dressCharacter.
					if (!item.entry) { item.slot = 15; }
				}
				primary.push_back(item);
			}
			// Their WoW items keep their slots; a Skyrim item's look fills a slot no WoW item of theirs takes.
			auto order = [](const auto& a, const auto& b) {
				if (static_cast<bool>(a.entry) != static_cast<bool>(b.entry)) { return a.entry != 0; }
				return a.form < b.form;
			};
			std::ranges::sort(primary, order);
			std::ranges::sort(additional, order);
			std::map<std::uint32_t, Character::Item> slots;
			for (auto i : primary) {
				if (i.inventoryType == 12 && slots.contains(12)) i.slot = 13;
				slots.try_emplace(i.slot, i);
			}
			for (const auto& i : additional) {
				slots.try_emplace(i.slot, i);
			}
			std::vector<Character::Item> out;
			for (const auto& [slot, i] : slots) { out.push_back(i); }
			return out;
		}

		// Their kit, the realm's equipment, into their inventory and on the player.
		void Give()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			for (const auto& worn : Character::Get().equipment) {
				const auto it = std::ranges::find(g_items, worn.entry, &Item::entry);
				if (it == g_items.end()) {
					continue;
				}
				player->AddObjectToContainer(it->form, nullptr, 1, nullptr);
				RE::ActorEquipManager::GetSingleton()->EquipObject(player, it->form, nullptr, 1, nullptr, false, false, false);
			}
			for (const auto& [entry, form] : g_reagents) {   // their bags: a stack of each reagent their spells use
				player->AddObjectToContainer(form, nullptr, kReagentStack, nullptr);
			}
			logger::info("gear: their kit given ({} items, {} reagents)", Character::Get().equipment.size(), g_reagents.size());
		}
	}

	void Init()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		LoadSkyrimLooks(data);
		Armory::Init();
		for (std::size_t i = 0; i < Armory::Abilities().size(); ++i) {
			for (std::size_t copy = 0; copy < Armory::Abilities()[i].size(); ++copy) {
				g_armoryCopies[Armory::Abilities()[i][copy]] = { static_cast<std::int32_t>(i), copy };
			}
		}
		for (const auto& [id, set] : Armory::Sets()) {
			for (const auto& bonus : set.bonuses) {
				if (auto* ability = bonus.ability ? data->LookupForm<RE::SpellItem>(bonus.ability, Armory::kPlugin) : nullptr) {
					g_armorySets[bonus.ability] = ability;
				}
			}
		}
		for (const auto& [entry, item] : Character::Gear()) {
			// Untyped, then cast: LookupForm<T> wants T's exact form type, and a bound object has none of its own.
			auto* found = data->LookupForm(item.form, kPlugin);
			auto* form  = found ? found->As<RE::TESBoundObject>() : nullptr;
			if (!form) {
				continue;
			}
			g_items.push_back({ entry, form, item.ability ? data->LookupForm<RE::SpellItem>(item.ability, kPlugin) : nullptr });
		}
		for (const auto& [entry, local] : Character::Reagents()) {
			if (auto* found = data->LookupForm(local, kPlugin); found && found->As<RE::TESBoundObject>()) {
				g_reagents[entry] = found->As<RE::TESBoundObject>();
			}
		}
		if (!Ready()) {
			logger::warn("gear: {} not loaded; the player wears the realm's equipment", kPlugin);
			return;
		}
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESEquipEvent>(&g_sink);
		logger::info("gear: {} WoW items are Skyrim items, {} more in {}", g_items.size(), Armory::Items().size(), Armory::kPlugin);
	}

	bool Ready() { return !g_items.empty() || !g_skyrimLooks.empty() || Armory::Ready(); }
	bool Given() { return g_given; }

	nlohmann::json WeaponDamages()
	{
		auto out = nlohmann::json::object();
		for (const auto& item : g_items) {
			if (const auto* weapon = item.form->As<RE::TESObjectWEAP>()) out[std::to_string(item.entry)] = weapon->attackDamage;
		}
		// The Armory's weapons the player carries (its other 1,700 are no tooltip of theirs; the service takes 256 at most).
		if (auto* player = RE::PlayerCharacter::GetSingleton(); player && Armory::Ready()) {
			for (const auto& [form, data] : player->GetInventory([](RE::TESBoundObject& o) { return o.IsWeapon() && Armory::Find(&o); })) {
				if (out.size() >= 200) break;
				if (data.first > 0) out[std::to_string(Armory::Find(form)->entry)] = form->As<RE::TESObjectWEAP>()->attackDamage;
			}
		}
		return out;
	}
	std::uint32_t EntryOf(const RE::TESBoundObject* a_form)
	{
		const auto* item = Find(a_form);
		if (item) return item->entry;
		if (const auto* armory = Armory::Find(a_form)) return armory->entry;
		for (const auto& [entry, form] : g_reagents) if (form == a_form) return entry;
		return 0;
	}

	RE::TESBoundObject* FormOf(std::uint32_t a_entry)
	{
		if (const auto it = std::ranges::find(g_items, a_entry, &Item::entry); it != g_items.end()) return it->form;
		const auto* armory = Armory::Find(a_entry);
		return armory ? armory->form : nullptr;
	}

	// Read instances for every request: crafting changes extra lists without changing
	// the base form/outfit. Never retain inventory pointers across frames.
	nlohmann::json Modifiers()
	{
		auto out = nlohmann::json::object();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return out;
		out["enchantments"] = nlohmann::json::array();
		if (!player->GetEquippedObject(false) || !player->GetEquippedObject(false)->IsWeapon()) {
			// Bare hands: their race's unarmed damage (the UnarmedDamage actor value; Skyrim has no unarmed skill, so the
			// One-handed multiplier stands in, as the engine's H2H uses no skill at all), never the WoW 1-2 fist scaled.
			out["weapon_entry"] = 0;
			out["weapon_skill"] = "OneHanded";
			out["weapon_damage"] = std::max(0.f, player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kUnarmedDamage));
			out["weapon_unarmed"] = true;
		}
		for (const auto& [form, data] : player->GetInventory()) {
			if (data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
			auto visit = [&](RE::ExtraDataList* extra) {
				RE::InventoryEntryData entry(form, 1);
				if (extra) entry.AddExtraList(extra);
				if (form == player->GetEquippedObject(false) && form->IsWeapon() &&
					(!extra || extra->HasType<RE::ExtraWorn>())) {
					RE::InventoryEntryData plain(form, 1);
					const float base = player->GetDamage(&plain);
					out["weapon_entry"] = EntryOf(form);
					const auto type = static_cast<int>(form->As<RE::TESObjectWEAP>()->GetWeaponType());
					out["weapon_skill"] = type == 7 || type == 9 ? "Marksman" : type == 5 || type == 6 ? "TwoHanded" : "OneHanded";
					out["temper_multiplier"] = base > 0 ? player->GetDamage(&entry) / base : 1.f;
					// Their swing deals the weapon's Skyrim damage at their skill (service/melee.py native_factor): a WoW
					// weapon's or a vanilla one's alike. Without it the service fell back to scaling the whole WoW roll,
					// attack power included, so a Daedric warhammer swung as WoW's 1-2 fist (balance review, October 16).
					out["weapon_damage"] = form->As<RE::TESObjectWEAP>()->attackDamage;
				}
				// Armour enchantments are applied by Skyrim on equip. Mirror their
				// relevant AVs to the service; intrinsic WoW abilities are excluded.
				if (!form->IsArmor()) return;
				if (auto* enchant = entry.GetEnchantment()) for (auto* effect : enchant->effects) {
					if (!effect || !effect->baseEffect) continue;
					out["enchantments"].push_back({ {"av", static_cast<int>(effect->baseEffect->data.primaryAV)},
						{"magnitude", effect->GetMagnitude() * (effect->baseEffect->IsDetrimental() ? -1.f : 1.f)} });
				}
			};
			bool visited = false;
			if (data.second->extraLists) for (auto* extra : *data.second->extraLists) {
				if (extra && (extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>())) { visit(extra); visited = true; }
			}
			if (!visited) visit(nullptr);
		}
		// Their fortified skills as Skyrim has them now, from anything (enchantments, potions, blessings, shrines): the
		// service prefers these to the worn enchantments' sum for the same values.
		auto& live = out["live"] = nlohmann::json::object();
		for (const auto value : { RE::ActorValue::kOneHandedModifier, RE::ActorValue::kTwoHandedModifier, RE::ActorValue::kMarksmanModifier,
				 RE::ActorValue::kAlterationModifier, RE::ActorValue::kConjurationModifier, RE::ActorValue::kDestructionModifier,
				 RE::ActorValue::kIllusionModifier, RE::ActorValue::kRestorationModifier }) {
			live[std::to_string(static_cast<int>(value))] = player->AsActorValueOwner()->GetActorValue(value);
		}
		return out;
	}

	// WoW swings bypass Skyrim's native weapon hit. Deliver the actual native
	// enchantment on a landed humanoid main-hand hit and spend its instance charge.
	void EnchantHit(RE::Actor* target)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !target) return;
		auto* hand = player->GetEquippedObject(false);
		for (const auto& [form, data] : player->GetInventory()) {
			if (form != hand || !form->IsWeapon() || !data.second || !data.second->extraLists) continue;
			for (auto* extra : *data.second->extraLists) {
				if (!extra || !extra->HasType<RE::ExtraWorn>()) continue;
				RE::InventoryEntryData entry(form, 1); entry.AddExtraList(extra);
				auto* enchant = entry.GetEnchantment();
				if (!enchant || enchant->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) return;
				const auto* custom = extra->GetByType<RE::ExtraEnchantment>();
				const auto* base = form->As<RE::TESEnchantableForm>();
				const float capacity = custom ? custom->charge : base ? base->amountofEnchantment : 0;
				auto* charge = extra->GetByType<RE::ExtraCharge>();
				const float remaining = charge ? charge->charge : capacity;
				if (capacity > 0 && remaining <= 0) return;
				auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
				if (!caster) return;
				const float cost = enchant->CalculateMagickaCost(player);
				caster->CastSpellImmediate(enchant, false, target, 1.f, false, 0.f, player);
				if (capacity > 0) {
					if (!charge) { charge = new RE::ExtraCharge(); extra->Add(charge); }
					charge->charge = std::max(0.f, remaining - std::max(0.f, cost));
					player->RefreshEquippedActorValueCharge(form, extra, false);
				}
				return;
			}
		}
	}

	bool UseEffect(RE::TESBoundObject* form, RE::ExtraDataList* extra)
	{
		const auto entry = EntryOf(form);
		const auto it = Character::Gear().find(entry);
		if (it == Character::Gear().end() || !it->second.useEntry) {
			if (!entry) return false;
			Cast::UseItem(entry);
			return true;
		}
		const auto& item = it->second;
		auto error = [](const char* text) { WowUi::Run(std::format("__WBFire('UI_ERROR_MESSAGE', {})", LuaQuote(text))); };
		if (!Cast::ItemReady(Cast::kItemTimer | entry) || (item.useReadyCategory && !Cast::ItemReady(Cast::kItemCategory | item.useReadyCategory))) {
			WowUi::Run("__WBFire('UI_ERROR_MESSAGE', ERR_ITEM_COOLDOWN)"); return true;   // the client's own refusal
		}
		// Do not discard a player's crafted instance while changing its base item.
		if (extra && (extra->HasType<RE::ExtraEnchantment>() || extra->HasType<RE::ExtraHealth>() || extra->HasType<RE::ExtraTextDisplayData>())) {
			error("This modified item cannot be transformed."); return true;
		}
		const auto other = std::ranges::find(g_items, item.useEntry, &Item::entry);
		if (other == g_items.end()) { error("The transformed item is unavailable."); return true; }
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* equip = RE::ActorEquipManager::GetSingleton();
		const bool worn = extra && (extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>());
		if (worn) equip->UnequipObject(player, form, extra, 1, nullptr, false, true, false, true);
		player->RemoveItem(form, 1, RE::ITEM_REMOVE_REASON::kRemove, extra, nullptr);
		player->AddObjectToContainer(other->form, nullptr, 1, nullptr);
		if (worn) equip->EquipObject(player, other->form, nullptr, 1, nullptr, false, true, false, true);
		// The two staves share item category 1031's thirty-minute cooldown; their own spell timers are zero.
		Cast::ItemCooldown(Cast::kItemTimer | entry, item.useItemCooldown);
		if (item.useCategory) Cast::ItemCooldown(Cast::kItemCategory | item.useCategory, item.useCategoryCooldown);
		g_dirty = true;
		return true;
	}

	void Reset()
	{
		g_given = false;
		g_unannounced = false;
	}

	void OnGameLoaded()
	{
		if (!Integration::Active()) return;
		g_unannounced = false;
		if (!Ready()) {
			return;
		}
		if (!g_given && !g_items.empty() && Character::Get().classId == 11) {   // WoWBridge.esp's kit is a Druid's
			Give();
			g_given = true;
		}
		g_known = false;
		g_nextRead = 0;
		g_dirty = true;
	}

	std::map<std::uint32_t, int> ReagentCounts()
	{
		std::map<std::uint32_t, int> counts;
		auto*                        player = RE::PlayerCharacter::GetSingleton();
		const auto                   inventory = player->GetInventoryCounts([](RE::TESBoundObject& a_object) {
            return std::ranges::any_of(g_reagents, [&](const auto& r) { return r.second == &a_object; });
        });
		for (const auto& [entry, form] : g_reagents) {
			const auto it = inventory.find(form);
			counts[entry] = it == inventory.end() ? 0 : it->second;
		}
		return counts;
	}

	void Consume(std::uint32_t a_entry, int a_count)
	{
		if (const auto it = g_reagents.find(a_entry); it != g_reagents.end() && a_count > 0) {
			RE::PlayerCharacter::GetSingleton()->RemoveItem(it->second, a_count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
		}
	}

	void Tick()
	{
		// The interface's item counts (a spell's "Reagents:" line, red when the player has none), as they change.
		if (static double next = 0; !g_reagents.empty() && Frame::Now() >= next) {
			next = Frame::Now() + 0.5;
			if (auto counts = ReagentCounts(); counts != g_shownCounts) {
				std::string lua;
				for (const auto& [entry, n] : counts) {
					lua += std::format("__WB.itemCounts[{}] = {} ", entry, n);
				}
				WowUi::Run(lua + "__WBFire('BAG_UPDATE', 0)");
				g_shownCounts = std::move(counts);
			}
		}
		if (const auto* player = RE::PlayerCharacter::GetSingleton(); g_unannounced && player->GetParentCell() && player->Is3DLoaded()) {
			OnGameLoaded();
		}
		// An equip event may precede the inventory/hand update. Reconcile periodically as well, so an
		// unchanged read at the event cannot leave the player in that outfit until another item is equipped.
		const auto* readyPlayer = RE::PlayerCharacter::GetSingleton();
		if (!Ready() || !readyPlayer || !readyPlayer->Is3DLoaded()) {
			return;
		}
		if (!g_dirty.exchange(false) && Frame::Now() < g_nextRead) {
			return;
		}
		g_nextRead = Frame::Now() + 0.25;
		auto worn = Equipped();
		if (g_known && worn == g_worn) {
			return;
		}
		g_worn = std::move(worn);
		g_known = true;
		Character::SetWorn(g_worn);
		Avatar::Redress();
		auto* player = RE::PlayerCharacter::GetSingleton();
		for (const auto& item : g_items) {   // intrinsic item stats while equipped
			const bool wearing = std::ranges::find(g_worn, item.entry, &Character::Item::entry) != g_worn.end();
			if (item.ability && wearing && !player->HasSpell(item.ability)) {
				player->AddSpell(item.ability);
			} else if (item.ability && !wearing && player->HasSpell(item.ability)) {
				player->RemoveSpell(item.ability);
			}
		}
		const auto wearing = [](std::uint32_t a_entry) {
			return std::ranges::any_of(g_worn, [a_entry](const auto& item) { return item.entry == a_entry; });
		};
		for (const auto& [key, set] : Character::Sets()) {
			const auto count = std::ranges::count_if(set.entries, wearing);
			for (const auto& bonus : set.bonuses) {
				auto* ability = bonus.ability ? RE::TESDataHandler::GetSingleton()->LookupForm<RE::SpellItem>(bonus.ability, kPlugin) : nullptr;
				if (!ability) continue;
				if (count >= bonus.threshold && !player->HasSpell(ability)) player->AddSpell(ability);
				else if (count < bonus.threshold && player->HasSpell(ability)) player->RemoveSpell(ability);
			}
		}
		// The Armory's bonuses: each worn item takes the next copy of its ability, so equal bonuses add up. Each set
		// bonus has its own ability even if another set uses the same spell. Unearned copies (including saved ones) go.
		std::map<std::int32_t, std::size_t> copies;
		for (const auto& i : g_worn) {
			if (const auto* armory = i.entry ? Armory::Find(i.entry) : nullptr; armory && armory->ability >= 0) ++copies[armory->ability];
		}
		const auto& added = player->GetActorRuntimeData().addedSpells;
		const std::set<RE::SpellItem*> held(added.begin(), added.end());
		const auto grant = [player, &held](RE::SpellItem* a_spell, bool a_wanted) {
			if (a_wanted && !held.contains(a_spell)) player->AddSpell(a_spell);
			else if (!a_wanted && held.contains(a_spell)) player->RemoveSpell(a_spell);
		};
		for (const auto& [spell, copy] : g_armoryCopies) {
			const auto it = copies.find(copy.first);
			grant(spell, it != copies.end() && copy.second < it->second);
		}
		for (const auto& [id, set] : Armory::Sets()) {
			const auto count = static_cast<std::uint32_t>(std::ranges::count_if(set.entries, wearing));
			for (const auto& bonus : set.bonuses) {
				if (const auto it = g_armorySets.find(bonus.ability); it != g_armorySets.end()) grant(it->second, count >= bonus.threshold);
			}
		}
		std::string list;
		for (const auto& i : g_worn) {
			list += std::format(" {}:{}{}", i.slot, i.displayId, i.entry ? "" : std::format("(form {:08X})", i.form));
		}
		logger::info("gear: the player wears {} WoW slots:{}", g_worn.size(), list);
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		const std::uint8_t given = g_given ? 1 : 0;
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion) || !a_intfc->WriteRecordData(given)) {
			logger::error("gear: the save record was not written");
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		std::uint8_t given = 0;
		if (a_length >= 1 && a_intfc->ReadRecordData(given)) {
			g_given = given != 0;
		}
	}
}
