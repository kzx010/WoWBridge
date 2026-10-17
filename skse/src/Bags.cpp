#include "Bags.h"
#include "NativeText.h"

#include "Armory.h"
#include "Frame.h"
#include "Character.h"
#include "Gear.h"
#include "Learning.h"
#include "LuaText.h"
#include "ui/WowUi.h"
#include "wowcore/bags.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

// Windows GDI's macro conflicts with CommonLib's default-object accessor.
#undef GetObject

namespace Bags
{
	namespace
	{
		using json = nlohmann::json;
		wowcore::BagPiles g_places;
		std::uint32_t g_revision = 0;
		std::map<std::string, std::pair<std::string,double>> g_uses;
		std::map<std::string, json> g_identities;
		std::map<std::uint32_t, json> g_wow;
		std::atomic<bool> g_dirty = true;
		std::uint32_t g_page = 1;
		std::atomic<std::uint32_t> g_epoch = 0;
		double g_next = 0;
		std::string g_last;
        struct EquipmentSwap {
            std::string incoming;
            std::uint32_t from=0;
            std::map<std::string,std::vector<int>> before;
            double expires=0;
        };
        std::vector<EquipmentSwap> g_equipmentSwaps;

		std::string Lua(const json& value)
		{
			if (value.is_object()) {
				std::string out="{";
				for (const auto& [k,v] : value.items()) out += "[" + LuaQuote(k) + "]=" + Lua(v) + ",";
				return out+"}";
			}
			if (value.is_array()) {
				std::string out="{";
				for (const auto& v : value) out += Lua(v)+",";
				return out+"}";
			}
			if (value.is_string()) return LuaQuote(value.get<std::string>());
			return value.is_null() ? "nil" : value.dump();
		}

		struct Stack
		{
			RE::TESBoundObject* form = nullptr;
			RE::ExtraDataList* extra = nullptr; // never retained across a task/frame
			json identity, item;
			std::string key;
		};

		std::string Kind(RE::TESBoundObject* form)
		{
			switch (form->GetFormType()) {
			case RE::FormType::Weapon: return "Weapon";
			case RE::FormType::Armor: return "Armor";
			case RE::FormType::Ammo: return "Ammo";
			case RE::FormType::Book: return "Book";
			case RE::FormType::Scroll: return "Scroll";
			case RE::FormType::Ingredient: return "Ingredient";
			case RE::FormType::SoulGem: return "SoulGem";
			case RE::FormType::KeyMaster: return "KeyMaster";
			case RE::FormType::AlchemyItem: return "Potion";
			default: return "Miscellaneous";
			}
		}

		void Effects(RE::MagicItem* magic, json& item)
		{
			if (!magic) return;
            // Skyrim expands native descriptions (including Survival globals and
            // indexed effect placeholders) before the plain-text client sees them.
            const auto* survival=RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESGlobal>(0x314A,"Update.esm"); // Survival_ModeEnabledShared
            const bool survivalEnabled=survival && survival->value>0;
            RE::BSString native;
            RE::MagicSystem::GetMagicItemDescription(native,magic,"","");
            const auto description=NativeText::ItemDescription(native.c_str() ? native.c_str() : "",survivalEnabled);
            if (!description.empty()) { item["effects"].push_back(description); return; }
			for (auto* e : magic->effects) {
				if (!e || !e->baseEffect) continue;
				std::string text = e->baseEffect->magicItemDescription.c_str();
				if (text.empty()) text = e->baseEffect->GetName();
				auto replace = [&](std::string_view token, const std::string& value) {
					std::size_t at=0;
					while ((at=text.find(token,at)) != std::string::npos) {
						text.replace(at,token.size(),value); at+=value.size();
					}
				};
				replace("<mag>",std::format("{:g}",e->GetMagnitude()));
				replace("<dur>",std::to_string(e->GetDuration()));
				replace("<area>",std::to_string(e->GetArea()));
				item["effects"].push_back(NativeText::ItemDescription(text,survivalEnabled));
			}
		}

		Stack Describe(RE::TESBoundObject* form, RE::ExtraDataList* extra, int count)
		{
			auto* player=RE::PlayerCharacter::GetSingleton();
			RE::InventoryEntryData entry(form, count);
			if (extra) entry.AddExtraList(extra);
			const bool stolen = !entry.IsOwnedBy(player);
			const auto* owner = stolen ? entry.GetOwner() : nullptr;
			const auto* enchant = entry.GetEnchantment();
			const auto* health = extra ? extra->GetByType<RE::ExtraHealth>() : nullptr;
			const auto* soul = extra ? extra->GetByType<RE::ExtraSoul>() : nullptr;
			const auto* poison = extra ? extra->GetByType<RE::ExtraPoison>() : nullptr;
			// Worn/favourite/charge flags do not change stack identity. Equivalent Skyrim instances stay one stack;
			// customised names, enchantments, tempering and soul contents distinguish otherwise identical forms.
			const char* rawName=Named(form,extra) ? entry.GetDisplayName() : nullptr;   // never "<Missing Name>"
			if (!rawName || !*rawName) rawName=form->GetName();
			const std::string name=rawName && *rawName ? rawName : std::format("{} {:08X}",Kind(form),form->GetFormID());
			json identity = { {"form",form->GetFormID()}, {"enchant",enchant ? enchant->GetFormID() : 0},
				{"name",name}, {"health",health ? health->health : 1.f},
				{"poison",poison && poison->poison ? poison->poison->GetFormID() : 0},
				{"soul",soul ? static_cast<int>(soul->GetContainedSoul()) : 0} };
			if (owner) identity["owner"] = owner->GetFormID();
			Stack s{form, extra, identity, json::object(), identity.dump()};
			auto& i=s.item;
			i = { {"key",s.key}, {"form",form->GetFormID()}, {"name",name}, {"count",count},
				{"worn",entry.IsWorn()}, {"kind",Kind(form)}, {"weight",entry.GetWeight()}, {"value",entry.GetValue()},
				{"stolen",stolen}, {"enchanted",entry.IsEnchanted()}, {"effects",json::array()}, {"keywords",json::array()} };
			std::uint32_t instance = 2166136261u;
			for (const unsigned char c : s.key) { instance = (instance ^ c) * 16777619u; }
			i["instance"] = instance;
			i["enchantment"] = enchant ? enchant->GetFormID() : 0;
			auto* keywords = form->As<RE::BGSKeywordForm>();
			if (keywords) {
				// Explicit precedence, as the reviewed table's multi-keyword records should read deterministically.
				for (const auto* keyword : {"VendorItemFood","VendorItemGem","VendorItemOreIngot","VendorItemAnimalHide",
						 "VendorItemAnimalPart","VendorItemIngredient","VendorItemBook","VendorItemScroll",
						 "VendorItemJewelry","VendorItemPotion"}) {
					if (keywords->HasKeywordString(keyword)) i["keywords"].push_back(keyword);
				}
			}
			if (auto* weapon=form->As<RE::TESObjectWEAP>()) {
				i["weapon"]=static_cast<int>(weapon->GetWeaponType());
				i["damage"]=player->GetDamage(&entry);
				RE::InventoryEntryData plain(form,1);
				i["plainDamage"]=player->GetDamage(&plain);   // untempered: this copy's tempering is damage / plainDamage
				i["baseDamage"]=weapon->attackDamage;
				const char* names[]={"Weapon","Sword","Dagger","Axe","Mace","Greatsword","Battleaxe","Bow","Staff","Crossbow"};
				const auto type=static_cast<unsigned>(weapon->GetWeaponType());
				i["typeName"]=names[type<10 ? type : 0];
				i["slotName"]=type==5 || type==6 || type==7 || type==8 || type==9 ? "Two-Hand" : "One-Hand";
				if (entry.IsWorn()) {
					i["inventorySlots"]=json::array();
					if (type==7 || type==9) {
						if (player->GetEquippedObject(false)==form || player->GetEquippedObject(true)==form) i["inventorySlots"].push_back(18);
					} else {
						if (player->GetEquippedObject(false)==form && (!extra || extra->HasType<RE::ExtraWorn>())) i["inventorySlots"].push_back(16);
						if (player->GetEquippedObject(true)==form && (!extra || extra->HasType<RE::ExtraWornLeft>()) && (player->GetEquippedObject(false)!=form || type<5 || type==8)) i["inventorySlots"].push_back(17);
					}
				}
			} else if (auto* armor=form->As<RE::TESObjectARMO>()) {
				using S=RE::BGSBipedObjectForm::BipedObjectSlot;
				std::string slot="unmapped", nameSlot="Armor";
				for (const auto& [mask,key,title] : {
						std::tuple{S::kShield,"shield","Off Hand"}, {S::kRing,"ring","Finger"}, {S::kAmulet,"amulet","Neck"},
						{S::kHead,"head","Head"}, {S::kHair,"head","Head"}, {S::kCirclet,"head","Head"},
						{S::kBody,"body","Chest"}, {S::kHands,"hands","Hands"}, {S::kFeet,"feet","Feet"},
						{S::kForearms,"wrists","Wrist"}, {S::kModPelvisPrimary,"waist","Waist"},
						{S::kModPelvisSecondary,"legs","Legs"}, {S::kModBack,"back","Back"}, {S::kModShoulder,"shoulders","Shoulder"} }) {
					if (armor->HasPartOf(mask)) { slot=key; nameSlot=title; break; }
				}
				i["armorSlot"]=slot; i["slotName"]=nameSlot;
				if (entry.IsWorn()) {
					const std::map<std::string,int> slots={{"head",1},{"amulet",2},{"shoulders",3},{"body",5},{"waist",6},
						{"legs",7},{"feet",8},{"wrists",9},{"hands",10},{"ring",11},{"back",15},{"shield",17}};
					i["inventorySlots"]=slots.contains(slot) ? json::array({slots.at(slot)}) : json::array();
				}
				i["armorType"]=static_cast<int>(armor->GetArmorType());
				i["typeName"]=armor->IsHeavyArmor() ? "Heavy Armor" : armor->IsLightArmor() ? "Light Armor" : "Clothing";
				if (armor->GetArmorRating()>0) i["armor"]=player->GetArmorValue(&entry);
				i["baseArmor"]=armor->GetArmorRating();
			} else if (form->As<RE::TESAmmo>()) {
				if (entry.IsWorn()) i["inventorySlots"]=json::array({0});
			} else if (auto* potion=form->As<RE::AlchemyItem>()) {
				if (potion->IsFood() || (keywords && keywords->HasKeywordString("VendorItemFood"))) {
					i["kind"]="Food"; i["typeName"]="Food";
				} else {
					i["potion"]=potion->IsPoison() ? "poison" : "default";
					if (!potion->IsPoison()) for (auto* e : potion->effects) {
						if (!e || !e->baseEffect) continue;
						const auto av=e->baseEffect->data.primaryAV;
						if (av==RE::ActorValue::kHealth) { i["potion"]="health"; break; }
						if (av==RE::ActorValue::kMagicka) { i["potion"]="magicka"; break; }
						if (av==RE::ActorValue::kStamina) { i["potion"]="stamina"; break; }
					}
					i["typeName"]=potion->IsPoison() ? "Poison" : "Potion";
				}
				Effects(potion,i);
			}
			Effects(const_cast<RE::EnchantmentItem*>(enchant),i);
			if (health && health->health > 1.f) {
				// Ask Skyrim for the quality suffix; its settings own all thresholds
				// and Legendary is open-ended. This also works with renamed items.
				RE::ExtraTextDisplayData display;
				const std::string tempered = display.GetDisplayName(form, health->health);
				const auto at = tempered.rfind('(');
				if (at != std::string::npos && tempered.back() == ')') i["temper"] = tempered.substr(at + 1, tempered.size() - at - 2);
			}
			i["temperFactor"] = health ? health->health : 1.f;
			if (const auto entryId=Gear::EntryOf(form) ? Gear::EntryOf(form) : Learning::EntryOf(form); entryId) {
				i["entry"]=entryId;
				const auto it=g_wow.find(entryId);
				const json armory=it==g_wow.end() ? Armory::Row(entryId) : json();   // WoWBridgeArmory.esp's, as gear.json's
				if (const auto& row=it!=g_wow.end() ? it->second : armory; row.is_object()) {
					i["display"]=row.value("displayid",0);
					if (row.contains("quality")) i["quality"]=row["quality"];
					if (row.contains("wow")) i["wow"]=row["wow"];
					if (row.contains("teaches")) i["teaches"]=row["teaches"];   // a book's rank, a mount item's mount
					if (row.contains("icon")) i["texture"]=row["icon"];   // an icon no 1.12 item display has
					if (row.contains("set")) i["set"]=row["set"];
					if (row.contains("skyrim")) i["skyrim"]=row["skyrim"];   // its converted armour/damage
					if (row.contains("use")) i["use"]=row["use"];
					if (entry.IsWorn() && row.contains("InventoryType")) {
						if (!form->As<RE::TESObjectWEAP>()) {
							const auto slot=Character::SlotOf(row["InventoryType"]);
							i["inventorySlots"]=slot<19 ? json::array({slot+1}) : json::array();
						}
					}
				}
			}
			return s;
		}

		std::vector<Stack> Gather()
		{
			std::map<std::string,Stack> grouped;
			auto* player=RE::PlayerCharacter::GetSingleton();
			if (!player) return {};
			for (const auto& [form,data] : player->GetInventory()) {
				if (!form || data.first<=0 || form->GetFormID()==0xF) continue;   // septims are their money (GetMoney), not a stack
				int remaining=data.first;
				auto add=[&](RE::ExtraDataList* extra,int count) {
					if (count<=0 || !Listed(form,extra)) return;
					auto s=Describe(form,extra,count);
					auto [it,inserted]=grouped.try_emplace(s.key,std::move(s));
					if (!inserted) {
						it->second.item["count"]=it->second.item["count"].get<int>()+count;
						if (extra && (extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>())) {
							it->second.extra=extra; it->second.item["worn"]=true;
							if (s.item.contains("inventorySlots")) {
								for (const auto& slot : s.item["inventorySlots"]) {
									auto& slots=it->second.item["inventorySlots"];
									if (slots.is_null()) slots=json::array();
									if (std::ranges::find(slots,slot)==slots.end()) slots.push_back(slot);
								}
							}
						}
					}
				};
				if (data.second && data.second->extraLists) for (auto* extra : *data.second->extraLists) {
					if (!extra || remaining<=0) continue;
					const int count=std::min(remaining,std::max(0,extra->GetCount()));
					add(extra,count); remaining-=count;
				}
				add(nullptr,remaining);
			}
			std::vector<Stack> out;
			for (auto& [key,s] : grouped) out.push_back(std::move(s));
			// First publication is deterministic; subsequent publications never sort/compact existing places.
			std::ranges::sort(out, [](const auto& a,const auto& b) {
				if (a.form->GetFormID()!=b.form->GetFormID()) return a.form->GetFormID()<b.form->GetFormID();
				return a.key<b.key;
			});
			return out;
		}

        std::map<std::string,std::vector<int>> WornSlots(const std::vector<Stack>& stacks)
        {
            std::map<std::string,std::vector<int>> slots;
            for (const auto& stack : stacks) if (stack.item.value("worn",false))
                for (const auto& slot : stack.item.value("inventorySlots",json::array())) slots[stack.key].push_back(slot.get<int>());
            return slots;
        }
        void RememberEquipmentSwap(const std::string& key)
        {
            // A character-to-character drag has no vacated bag destination.
            if (g_places.equippedSlot) return;
            const auto pile=g_places.piles.find(key);
            if (pile==g_places.piles.end()) return;
            const auto from=pile->second.pos ? pile->second.pos : g_places.origin;
            if (from) g_equipmentSwaps.push_back({pile->second.identity,from,WornSlots(Gather()),Frame::Now()+2});
        }

		void Reconcile(const std::vector<Stack>& stacks)
		{
			std::vector<std::pair<std::string,std::int32_t>> counts;
			std::map<std::string,json> identities;
			for (const auto& s : stacks) {
				const int n=s.item.at("count").get<int>();
				counts.emplace_back(s.key,n); identities[s.key]=s.identity;
				if (const auto use=g_uses.find(s.key); use!=g_uses.end()) {
					const auto removed=g_places.total(s.key)-n;
					if (removed>0 && Frame::Now()<=use->second.second) g_places.debit(use->second.first,static_cast<int>(removed));
					if (removed>0 || Frame::Now()>use->second.second) g_uses.erase(use);
				}
			}
			// An unambiguous changed identity retains all its piles and stable UI keys.
			for (const auto& s : stacks) if (!g_places.contains(s.key)) {
				std::vector<std::string> candidates;
				int newcomers=0;
				for (const auto& other : stacks) if (other.identity["form"]==s.identity["form"] && !g_places.contains(other.key)) ++newcomers;
				for (const auto& [key,id] : g_identities) if (id["form"]==s.identity["form"] && !identities.contains(key) && g_places.contains(key)) candidates.push_back(key);
				if (newcomers==1 && candidates.size()==1) g_places.rename(candidates[0],s.key);
			}
			g_places.reconcile(counts); g_identities=std::move(identities);
            const auto worn=WornSlots(stacks);
            for (auto it=g_equipmentSwaps.begin();it!=g_equipmentSwaps.end();) {
                const auto incoming=worn.find(it->incoming);
                const auto before=it->before.find(it->incoming);
                const bool confirmed=incoming!=worn.end() && incoming->second.size()>(before==it->before.end() ? 0 : before->second.size());
                if (!confirmed && Frame::Now()<it->expires) { ++it; continue; }
                if (confirmed) {
                    std::string outgoing; int priority=100;
                    for (const auto& [identity,slots] : it->before) {
                        if (identity==it->incoming) continue;
                        const auto after=worn.find(identity);
                        if (after!=worn.end() && after->second.size()>=slots.size()) continue;
                        // For a two-hand replacement, put the former main hand
                        // in the source place; displaced off-hand keeps its own.
                        const int slot=*std::min_element(slots.begin(),slots.end());
                        if (slot<priority) { outgoing=identity; priority=slot; }
                    }
                    if (!outgoing.empty()) g_places.returnEquipment(it->from,it->incoming,outgoing);
                }
                it=g_equipmentSwaps.erase(it);
            }
			for (auto it=g_uses.begin(); it!=g_uses.end();) {
				if (!g_identities.contains(it->first) || !g_places.piles.contains(it->second.first)) it=g_uses.erase(it); else ++it;
			}
			g_page=std::clamp(g_page,1u,g_places.pages());
		}

		void Publish(const std::vector<Stack>& stacks)
		{
			json list=json::array();
			for (const auto& [key,pile] : g_places.piles) {
				const auto s=std::ranges::find(stacks,pile.identity,&Stack::key);
				if (s==stacks.end()) continue;
				auto item=s->item;
				item["key"]=key; item["identity"]=pile.identity; item["pos"]=pile.pos; item["count"]=pile.count;
				list.push_back(std::move(item));
			}
			const auto lua="__WBInventory("+Lua(list)+","+std::to_string(g_page)+","+
				LuaQuote(g_places.held)+","+std::to_string(g_places.origin)+","+std::to_string(g_revision)+","+(g_places.equippedSlot ? std::to_string(*g_places.equippedSlot) : "nil")+")";
			if (lua!=g_last) { g_last=lua; WowUi::Run(lua); }
		}

		// Resolve each identity's actual extra lists afresh after every removal; a grouped Stack's
		// representative list alone cannot drop a quantity spanning several equivalent instances.
		std::vector<Stack> Parts(const std::string& identity)
		{
			std::vector<Stack> out;
			for (const auto& [form,data] : RE::PlayerCharacter::GetSingleton()->GetInventory()) {
				if (!form || data.first<=0) continue;
				int remaining=data.first;
				auto add=[&](RE::ExtraDataList* extra,int n) {
					if (n<=0) return;
					auto s=Describe(form,extra,n);
					if (s.key==identity) out.push_back(std::move(s));
				};
				if (data.second && data.second->extraLists) for (auto* extra : *data.second->extraLists) {
					if (!extra || remaining<=0) continue;
					const auto n=std::min(remaining,std::max(0,extra->GetCount()));
					add(extra,n); remaining-=n;
				}
				add(nullptr,remaining);
			}
			return out;
		}

		void DropHeld(const std::string& key,int count)
		{
			if (key!=g_places.held || !g_places.piles.contains(key)) return;
			const auto pile=g_places.piles.at(key);
			if (count!=pile.count) { WowUi::Run("__WBFire('UI_ERROR_MESSAGE', 'That stack has changed.')"); return; }
			auto* player=RE::PlayerCharacter::GetSingleton();
			auto parts=Parts(pile.identity);
			if (parts.empty()) return;
			const auto inventory=player->GetInventory();
			const auto entry=inventory.find(parts.front().form);
			if (entry==inventory.end() || !entry->second.second || entry->second.second->IsQuestObject()) {
				WowUi::Run("__WBFire('UI_ERROR_MESSAGE', 'You cannot drop quest items.')");
				return;
			}
			const auto location=player->GetPosition()+RE::NiPoint3{0.f,0.f,20.f};
			int remaining=pile.count;
			while (remaining>0) {
				parts=Parts(pile.identity);
				if (parts.empty()) break;
				// Prefer spare copies; a character drag selects only its original worn hand.
				const auto selected=g_places.equippedSlot;
				const auto found=std::ranges::find_if(parts,[&](const auto& part) {
					const auto slots=part.item.value("inventorySlots",json::array());
					return selected ? std::ranges::find(slots,json(*selected==12 ? 11 : *selected==14 ? 13 : *selected))!=slots.end() : !part.item.value("worn",false);
				});
				auto s=found==parts.end() ? parts.front() : *found;
				if (selected && found==parts.end()) break;
				const bool worn=s.item.value("worn",false);
				const auto* hand=s.extra && s.extra->HasType<RE::ExtraWornLeft>() ?
					RE::BGSDefaultObjectManager::GetSingleton()->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kLeftHandEquip) : nullptr;
				if (worn) {
					RE::ActorEquipManager::GetSingleton()->UnequipObject(player,s.form,s.extra,1,hand,false,true,false,true);
					// Unequip can replace/free an extra list; never pass its previous pointer to RemoveItem.
					const auto fresh=Parts(pile.identity);
					const auto same=std::ranges::find_if(fresh,[&](const auto& part) { return part.extra==s.extra && !part.item.value("worn",false); });
					const auto spare=std::ranges::find_if(fresh,[](const auto& part) { return !part.item.value("worn",false); });
					if (same!=fresh.end()) s=*same;
					else if (spare!=fresh.end()) s=*spare;
					else break;
				}
				const auto n=std::min(remaining,s.item.at("count").get<int>());
				const auto before=g_places.total(pile.identity);
				player->RemoveItem(s.form,n,RE::ITEM_REMOVE_REASON::kDropping,s.extra,nullptr,&location);
				const auto now=Gather();
				const auto live=std::ranges::find(now,pile.identity,&Stack::key);
				const int actual=live==now.end() ? 0 : live->item.at("count").get<int>();
				const auto removed=static_cast<int>(std::clamp<int64_t>(before-actual,0,n));
				if (!removed) {
					// A refused removal leaves the equipment unchanged. Resolve extras again.
					if (worn) {
						const auto fresh=Parts(pile.identity);
						if (!fresh.empty()) RE::ActorEquipManager::GetSingleton()->EquipObject(player,s.form,fresh.front().extra,1,hand,false,true,false);
					}
					break;
				}
				g_places.debit(key,removed); remaining-=removed;
			}
			if (remaining) WowUi::Run("__WBFire('UI_ERROR_MESSAGE', 'This item cannot be dropped.')");
			g_dirty=true;
		}

		// Re-resolve worn extras and hand on the main thread, never retain inventory pointers in UI commands.
		bool Equipment(const std::string& key, std::uint32_t slot, bool equip)
		{
			const auto pile=g_places.piles.find(key);
			if (pile==g_places.piles.end()) return false;
			auto parts=Parts(pile->second.identity);
			const auto found=std::ranges::find_if(parts,[&](const auto& s) {
				if (equip) return !s.item.value("worn",false);
				const auto slots=s.item.value("inventorySlots",json::array());
				return s.item.value("worn",false) && std::ranges::find(slots,json(slot==12 ? 11 : slot==14 ? 13 : slot))!=slots.end();
			});
			if (found==parts.end()) return false;
			auto* player=RE::PlayerCharacter::GetSingleton();
			const auto* hand=slot==17 && found->form->As<RE::TESObjectWEAP>() ?
				RE::BGSDefaultObjectManager::GetSingleton()->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kLeftHandEquip) : nullptr;
			auto* manager=RE::ActorEquipManager::GetSingleton();
			if (equip) {
				if (!found->form->As<RE::TESObjectWEAP>() && !found->form->As<RE::TESObjectARMO>() && !found->form->As<RE::TESAmmo>()) return false;
                RememberEquipmentSwap(key);
				manager->EquipObject(player,found->form,found->extra,1,hand,false,true,false);
			} else manager->UnequipObject(player,found->form,found->extra,1,hand,false,true,false,true);
			g_dirty=true;
			return true;
		}

		struct Sink : RE::BSTEventSink<RE::TESContainerChangedEvent>, RE::BSTEventSink<RE::TESEquipEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* e, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				const auto* p=RE::PlayerCharacter::GetSingleton();
				if (e && p && (e->oldContainer==p->GetFormID() || e->newContainer==p->GetFormID())) g_dirty=true;
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* e, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (e && e->actor && e->actor->IsPlayerRef()) g_dirty=true;
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_sink;
	}

	bool Named(RE::TESBoundObject* form, RE::ExtraDataList* extra)
	{
		if (const char* name=form ? form->GetName() : nullptr; name && *name) return true;
		const auto* text=extra ? extra->GetByType<RE::ExtraTextDisplayData>() : nullptr;
		return text && !text->displayName.empty();
	}

	bool Listed(RE::TESBoundObject* form, RE::ExtraDataList* extra)
	{
		return form && form->GetPlayable() && Named(form,extra);
	}

	nlohmann::json DescribeItem(RE::TESBoundObject* form, RE::ExtraDataList* extra, int count)
	{
		return Describe(form,extra,count).item;
	}

	nlohmann::json Sale(const std::string& key)
	{
		auto stacks=Gather(); Reconcile(stacks);
		const auto pile=g_places.piles.find(key);
		if (pile==g_places.piles.end()) return nullptr;
		return {{"identity",pile->second.identity},{"count",pile->second.count}};
	}

	void Sold(const std::string& key, int count)
	{
		g_places.debit(key,count);
		auto stacks=Gather(); Reconcile(stacks); g_last.clear(); Publish(stacks);
	}

	void Init()
	{
		auto* source=RE::ScriptEventSourceHolder::GetSingleton();
		source->AddEventSink<RE::TESContainerChangedEvent>(&g_sink);
		source->AddEventSink<RE::TESEquipEvent>(&g_sink);
		try {
			std::ifstream file("Data/SKSE/Plugins/WoWBridge/gear.json");
			if (file) {
				const auto data=json::parse(file);
				for (const auto* section : {"items","reagents","books","mounts"}) for (const auto& [key,item] : data.value(section,json::object()).items()) {
					auto row = item;
					// Skyrim gear policy, including old packages; books and mount items keep their Skyrim level.
					if (row.contains("wow") && !row.contains("teaches")) row["wow"]["RequiredLevel"] = 0;
					g_wow[item.at("entry").get<std::uint32_t>()] = std::move(row);
				}
			}
		} catch (const std::exception& e) { logger::error("bags: gear metadata: {}",e.what()); }
	}

	std::string ItemLua(const nlohmann::json& items) { return Lua(items); }

	void Reset()
	{
		++g_epoch; g_equipmentSwaps.clear(); g_places={}; g_revision=0; g_uses.clear(); g_identities.clear(); g_last.clear(); g_page=1; g_next=0; g_dirty=true;
		WowUi::Run("__WBResetBags()");
	}

	void Tick()
	{
		auto* player=RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded() || !WowUi::Ready()) return;
		if (!g_dirty.exchange(false) && Frame::Now()<g_next) return;
		g_next=Frame::Now()+0.5;
		auto stacks=Gather(); Reconcile(stacks); Publish(stacks);
	}

	void Cursor(std::string action,std::string key,int count,std::uint32_t to,std::string expected,std::string splitKey,std::uint32_t revision)
	{
		SKSE::GetTaskInterface()->AddTask([action=std::move(action),key=std::move(key),count,to,
			expected=std::move(expected),splitKey=std::move(splitKey),revision,epoch=g_epoch.load()] {
			if (epoch!=g_epoch) return;
			auto stacks=Gather(); Reconcile(stacks);
			if (action=="grab") g_places.grab(key,count,splitKey);
			else if (action=="undress") {
				// Validate the selected worn instance; pickup only reserves its copy.
				const auto pile=g_places.piles.find(key);
				if (pile!=g_places.piles.end()) {
					const auto parts=Parts(pile->second.identity);
					if (std::ranges::any_of(parts,[&](const auto& s) {
						const auto slots=s.item.value("inventorySlots",json::array());
						return s.item.value("worn",false) && std::ranges::find(slots,json(to==12 ? 11 : to==14 ? 13 : to))!=slots.end();
					})) g_places.grabEquipped(key,to,splitKey);
				}
			} else if (action=="unequip") Equipment(key,to,false);
			else if (action=="equip") {
				if (g_places.held==key) {
					if (g_places.equippedSlot && *g_places.equippedSlot==to) g_places.cancel();
					else if ((!g_places.equippedSlot || Equipment(key,*g_places.equippedSlot,false)) && Equipment(key,to,true)) g_places.cancel();
				}
			} else if (action=="place") {
				// Check placement first: stale/invalid drops must leave the outfit intact.
				auto placed=g_places;
				if (placed.place(key,to,expected) && (!g_places.equippedSlot || Equipment(key,*g_places.equippedSlot,false)))
					g_places=std::move(placed);
			}
			else if (action=="cancel") g_places.cancel();
			else if (action=="drop") DropHeld(key,count);
			g_revision=std::max(g_revision,revision);
			stacks=Gather(); Reconcile(stacks); g_last.clear(); Publish(stacks);
		});
	}

	void Use(std::string key, bool effect)
	{
		SKSE::GetTaskInterface()->AddTask([key=std::move(key),effect,epoch=g_epoch.load()] {
			if (epoch!=g_epoch) return;
			auto stacks=Gather(); Reconcile(stacks);
			const auto pile=g_places.piles.find(key);
			if (pile==g_places.piles.end()) { g_last.clear(); Publish(stacks); return; }
			const auto parts=Parts(pile->second.identity);
			if (parts.empty()) { g_last.clear(); Publish(stacks); return; }
			const auto spare=std::ranges::find_if(parts,[](const auto& part) { return !part.item.value("worn",false); });
			const auto it=spare==parts.end() ? parts.begin() : spare;
			auto* player=RE::PlayerCharacter::GetSingleton();
			auto* form=it->form;
			if (effect) {
				if (!Gear::UseEffect(form,it->extra)) WowUi::Run("__WBFire('UI_ERROR_MESSAGE', SPELL_FAILED_SPELL_UNAVAILABLE)");
				g_dirty=true; return;
			}
			auto* manager=RE::ActorEquipManager::GetSingleton();
			if (form->As<RE::AlchemyItem>() || form->As<RE::TESObjectWEAP>() || form->As<RE::TESObjectARMO>() || form->As<RE::TESAmmo>()) {
				const auto* left=it->extra && it->extra->HasType<RE::ExtraWornLeft>() ?
					RE::BGSDefaultObjectManager::GetSingleton()->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kLeftHandEquip) : nullptr;
				if (it->item.value("worn",false)) manager->UnequipObject(player,form,it->extra,1,left,false,false,false);
				else {
					if (form->As<RE::AlchemyItem>()) g_uses[it->key]={key,Frame::Now()+2.0};
                    if (!form->As<RE::AlchemyItem>()) RememberEquipmentSwap(key);
					manager->EquipObject(player,form,it->extra,1,nullptr,false,false,false);
				}
			} else if (Learning::EntryOf(form)) {   // a book or mount item: learned and used up, or the client's refusal
				g_uses[it->key]={key,Frame::Now()+2.0};
				Learning::Use(form,it->extra);
			} else {
				WowUi::Run("__WBFire('UI_ERROR_MESSAGE', 'This item cannot be used from a bag.')");
			}
			g_dirty=true;
		});
	}

	void Page(std::uint32_t page)
	{
		SKSE::GetTaskInterface()->AddTask([page,epoch=g_epoch.load()] {
			if (epoch==g_epoch) g_page=std::clamp(page,1u,g_places.pages());
		});
	}

	void Save(SKSE::SerializationInterface* intfc)
	{
		auto stacks=Gather(); Reconcile(stacks); g_places.cancel(); g_page=std::clamp(g_page,1u,g_places.pages()); g_last.clear(); Publish(stacks);
		json rows=json::array();
		for (const auto& [key,pile] : g_places.piles) rows.push_back({{"key",key},{"identity",g_identities.at(pile.identity)}, {"pos",pile.pos},{"count",pile.count}});
		const auto text=json({{"page",g_page},{"places",rows}}).dump();
		if (!intfc->OpenRecord(kRecord,kRecordVersion) || !intfc->WriteRecordData(text.data(),static_cast<std::uint32_t>(text.size())))
			logger::error("bags: places were not saved");
	}

	void Load(SKSE::SerializationInterface* intfc,std::uint32_t length,std::uint32_t version)
	{
		if (!length || length>4*1024*1024) { logger::error("bags: damaged record length"); return; }
		std::string text(length,'\0');
		if (intfc->ReadRecordData(text.data(),length)!=length) { logger::error("bags: unreadable record"); return; }
		try {
			const auto data=json::parse(text);
			std::map<std::string,wowcore::BagPiles::Pile> places;
			std::map<std::string,json> ids;
			for (const auto& row : data.at("places")) {
				auto id=row.at("identity");
				for (const auto* field : {"form","enchant","poison","owner"}) {
					if (!id.contains(field)) continue;
					const auto old=id.at(field).get<RE::FormID>();
					RE::FormID resolved=0;
					if (old && !intfc->ResolveFormID(old,resolved)) { id=json(); break; }
					id[field]=resolved;
				}
				if (id.is_null()) continue; // an uninstalled plugin's items cannot occupy a slot
				if (!id.at("form").get<RE::FormID>() || !id.at("name").is_string() ||
					!id.at("health").is_number() || !id.at("soul").is_number_integer()) throw std::runtime_error("invalid identity");
				const auto identity=id.dump();
				const auto key=version==1 ? identity : row.at("key").get<std::string>();
				if (!row.at("pos").is_number_integer()) throw std::runtime_error("invalid place");
				const auto pos=row.at("pos").get<std::int64_t>();
				if (pos<1 || pos>1000000) throw std::runtime_error("invalid place");
				if (version!=1 && !row.at("count").is_number_integer()) throw std::runtime_error("invalid count");
				const auto count=version==1 ? 0 : row.at("count").get<std::int64_t>();
				if (version!=1 && (count<1 || count>INT32_MAX)) throw std::runtime_error("invalid count");
				if (!places.emplace(key,wowcore::BagPiles::Pile{identity,static_cast<std::uint32_t>(pos),static_cast<std::int32_t>(count)}).second) throw std::runtime_error("duplicate pile key");
				ids[identity]=id;
			}
			const auto page=data.value("page",1u); // parse before committing any loaded state
			if (!g_places.restore(places,version==1)) throw std::runtime_error("invalid places");
			g_identities=std::move(ids); g_page=std::clamp(page,1u,g_places.pages());
			g_dirty=true; g_last.clear();
		} catch (const std::exception& e) { logger::error("bags: damaged co-save: {}",e.what()); }
	}
}
