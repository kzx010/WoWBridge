#include "ui/WowUi.h"
#include "Integration.h"
#include "UiProfileState.h"
#include "ColdPersistence.h"
#include "ActionState.h"

#include "Character.h"
#include "Avatar.h"

#include "Audio.h"
#include "Config.h"
#include "Bridge.h"
#include "Bags.h"
#include "Merchant.h"
#include "Trainers.h"
#include "Loot.h"
#include "Taxi.h"
#include "Input.h"
#include "render/WowModels.h"
#include "ui/imgui_impl_dx11.h"

#include "addons/lua_engine.hpp"
#include "core/app_clock.hpp"
#include "../../../wowcore/compat/rendering/vk_context.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/embedded.hpp"
// Keep these coupled APIs from this checkout, including in the shared-PCH syntax check.
#include "../../../wowcore/include/wowcore/mpq.hpp"
#include "../../../wowcore/include/wowcore/minimap.hpp"
#include "../../../wowcore/include/wowcore/ui_session.hpp"
#include "wowcore/wowapi.hpp"
#include "pipeline/blp_loader.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <d3d11.h>
#include <imgui.h>

namespace wowcore
{
	extern const EmbeddedFile kSliceState[];  // wowcore/tests/fixtures/elunara.lua (skse/CMakeLists.txt)
}

namespace WowUi
{
	namespace
	{
		// WoW art as D3D11 textures, mipmapped: the client samples minified UI art (an icon drawn at 34 px from 64)
		// from its mips, and the backend's sampler is trilinear.
		class DxTextures final : public wowee::rendering::VkContext
		{
		public:
			~DxTextures() override
			{
				for (auto* view : views) view->Release();
			}

			ID3D11Device*        device = nullptr;
			ID3D11DeviceContext* context = nullptr;

			VkDescriptorSet uploadImGuiTexture(const std::uint8_t* a_rgba, int a_width, int a_height) override
			{
				D3D11_TEXTURE2D_DESC d{};
				d.Width = static_cast<UINT>(a_width), d.Height = static_cast<UINT>(a_height);
				d.MipLevels = 0, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM, d.SampleDesc.Count = 1;
				d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
				ID3D11Texture2D* tex = nullptr;
				if (FAILED(device->CreateTexture2D(&d, nullptr, &tex))) {
					return VK_NULL_HANDLE;
				}
				context->UpdateSubresource(tex, 0, nullptr, a_rgba, static_cast<UINT>(a_width) * 4, 0);
				D3D11_SHADER_RESOURCE_VIEW_DESC v{};
				v.Format = d.Format, v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = static_cast<UINT>(-1);
				ID3D11ShaderResourceView* srv = nullptr;
				const HRESULT hr = device->CreateShaderResourceView(tex, &v, &srv);
				tex->Release();
				if (FAILED(hr)) {
					return VK_NULL_HANDLE;
				}
				context->GenerateMips(srv);
				views.push_back(srv);
				return reinterpret_cast<VkDescriptorSet>(srv);
			}

			bool updateImGuiTexture(VkDescriptorSet a_texture, const std::uint8_t* a_rgba, int a_width, int a_height) override
			{
				auto* srv=reinterpret_cast<ID3D11ShaderResourceView*>(a_texture);
				ID3D11Resource* resource=nullptr;
				srv->GetResource(&resource);
				if (!resource) return false;
				context->UpdateSubresource(resource,0,nullptr,a_rgba,static_cast<UINT>(a_width)*4,0);
				resource->Release(); context->GenerateMips(srv); return a_height>0;
			}

		private:
			std::vector<ID3D11ShaderResourceView*> views;  // held for the session's life
		};

		// 1.12's cursors: the interface names them as SetCursor("CAST_CURSOR"); the art is Interface\Cursor\<Name>.blp,
		// "_ERROR" ones the Unable<Name> art. Drawn at a fixed 32 px with the hot spot at the top left, as the client's
		// hardware cursor.
		std::string CursorFile(std::string a_name)
		{
			if (a_name.ends_with("_CURSOR")) {
				a_name.resize(a_name.size() - 7);
			}
			bool unable = false;
			if (a_name.ends_with("_ERROR")) {
				a_name.resize(a_name.size() - 6), unable = true;
			}
			static const std::map<std::string, std::string> kNames = {
				{ "POINT", "Point" }, { "ATTACK", "Attack" }, { "CAST", "Cast" }, { "INTERACT", "Interact" },
				{ "SPEAK", "Speak" }, { "PICKUP", "Pickup" }, { "ITEM", "Item" }, { "LOOT", "LootAll" }, { "BUY", "Buy" },
				{ "REPAIR", "Repair" }, { "INSPECT", "Inspect" }, { "MAIL", "Mail" }, { "TAXI", "Taxi" },
				{ "TRAINER", "Trainer" }, { "MINE", "Mine" }, { "HERB", "GatherHerbs" }, { "SKIN", "Skin" },
				{ "PICKLOCK", "PickLock" }
			};
			const auto it = kNames.find(a_name);
			const std::string file = it == kNames.end() ? "Point" : it->second;
			return std::string(R"(Interface\Cursor\)") + (unable ? "Unable" : "") + file + ".blp";
		}

		struct State
		{
			const Avatar::Loaded* characterLook=nullptr;
			std::shared_ptr<wowcore::Mpq>        mpq;
			std::unique_ptr<wowcore::ClientDb>   db;
			DxTextures                           textures;
			std::unique_ptr<wowcore::UiSession>  ui;
			wowcore::MinimapAtlas                 minimap;
			std::chrono::steady_clock::time_point last;
			double                               clock = 0;
			bool                                 failed = false;
			bool                                 reload = false;
			std::string                          uiCursor;      // the interface's SetCursor, "" after ResetCursor
			std::string                          spellCursor;   // the spell held by PickupSpell/PickupAction/PlaceAction
			std::map<std::string, ImTextureID>   cursors;       // file -> texture
		};
		State* g = nullptr;
		std::atomic<bool> g_ready = false;

		std::mutex         g_session;    // the interface's Lua state: the render's frame, or Eval from the main thread
		std::mutex         g_exchange;   // main thread <-> render
		std::string        g_snapshot;   // latest published, not yet applied
		std::vector<Click> g_clicks;
		std::atomic<bool>  g_overUi = false;
		std::string        g_worldCursor;   // from the main thread
		std::vector<std::function<void()>> g_tasks;   // main -> render, run in order before the next frame
		std::vector<Command>     g_commands;    // render -> main
        UiProfile::State        g_profile; // main thread, persisted in the co-save
        std::string             g_renderProfile{UiProfile::Legacy}; // protected by g_session
        std::optional<std::string> g_nextProfile; // protected by g_exchange
        bool                    g_profileReload = false;
        std::atomic<std::uint64_t> g_generation{0};

        std::string NewProfileKey()
        {
            std::random_device random;
            return std::format("character-{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
        }
        void RequestProfile()
        {
            std::lock_guard lock(g_exchange);
            g_nextProfile = g_profile.key;
            g_ready = false; // stop publishing to the old character's interface
        }
		Input::Buttons     g_lastButtons;

		// What the interface keeps between sessions, where the client keeps it (its WTF folder): key bindings
		// (bindings-cache.wtf; the account's, or the character's under its name), the cvars the player changed
		// (config-cache.wtf) and the interface's saved variables. In My Games\Skyrim Special Edition\WoWBridge\WTF.
		std::filesystem::path Wtf(std::string_view a_file, bool a_character = false)
		{
			auto dir = logger::log_directory()->parent_path() / "WoWBridge" / "WTF";
			if (Config::Get().autorun) dir /= "Tests";
            return (a_character ? UiProfile::Directory(dir, g_renderProfile) : dir) / a_file;
		}

		std::string ReadText(const std::filesystem::path& a_path)
		{
			std::ifstream in(a_path, std::ios::binary);
			return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string();
		}

		void WriteText(const std::filesystem::path& a_path, const std::string& a_text)
		{
			std::error_code ec;
			std::filesystem::create_directories(a_path.parent_path(), ec);
			std::ofstream(a_path, std::ios::binary | std::ios::trunc) << a_text;
		}

		std::map<std::string, std::string> g_cvars;   // the ones changed: config-cache.wtf (render thread)

		// Where her bar is kept between sessions; a self-test run keeps its own (it starts from the realm's bar).
		std::filesystem::path ActionsFile() { return Wtf(Config::Get().autorun ? "actions-test.lua" : "actions.lua", true); }
		std::filesystem::path LayoutFile() { return Wtf(Config::Get().autorun ? "layout-cache-test.txt" : "layout-cache.txt", true); }

		// Her action bar as Lua (__WB.actions), from the interface's own table: what the server keeps (character_action).
		std::string ActionsLua(lua_State* a_lua)
		{
			std::string lua = "__WB.actions = {";
			lua_getglobal(a_lua, "__WB");
			if (lua_istable(a_lua, -1)) {
				lua_getfield(a_lua, -1, "actions");
				if (lua_istable(a_lua, -1)) {
					lua_pushnil(a_lua);
					while (lua_next(a_lua, -2)) {
						if (lua_isnumber(a_lua, -2) && lua_istable(a_lua, -1)) {
							lua_getfield(a_lua, -1, "spell");
							if (lua_isnumber(a_lua, -1)) {
								ActionState::Append(lua, lua_tonumber(a_lua, -3), lua_tonumber(a_lua, -1));
							}
							lua_pop(a_lua, 1);
						}
						lua_pop(a_lua, 1);
					}
				}
				lua_pop(a_lua, 1);
			}
			lua_pop(a_lua, 1);
			return lua + " }";
		}

		// A WoW sound's samples, read once from the client's MPQs.
		Audio::Wav Samples(const std::string& a_file)
		{
			static std::map<std::string, Audio::Wav> cache;
			auto& wav = cache[a_file];
			if (!wav) {
				if (auto data = g->mpq->read(a_file)) {
					wav = std::make_shared<const std::vector<std::uint8_t>>(std::move(*data));
				}
			}
			return wav;
		}

		// One of a SoundEntries row's files, by weight.
		std::uint64_t PlayEntry(const wowcore::SoundInfo& a_sound, const RE::NiPoint3* a_at)
		{
			static std::mt19937 rng{ std::random_device{}() };
			if (a_sound.files.empty()) {
				return 0;
			}
			std::discrete_distribution<std::size_t> pick(a_sound.weights.begin(), a_sound.weights.end());
			const auto& file = a_sound.files[pick(rng)];
			const auto  wav = Samples(file);
			if (!wav) {
				return 0;
			}
			if (a_at) {
				const Audio::Where where{ *a_at, a_sound.minDistance > 0 ? a_sound.minDistance : 8.0f,
					a_sound.cutoff > 0 ? a_sound.cutoff : 40.0f };
				return Audio::Play(wav, a_sound.volume, a_sound.loop, &where);
			}
			return Audio::Play(wav, a_sound.volume, a_sound.loop);
		}

		// A WoW sound by its SoundEntries name or by file (the interface's PlaySound / PlaySoundFile).
		void PlayWowSound(std::string_view a_name, bool a_isFile)
		{
			if (a_isFile) {
				if (const auto wav = Samples(std::string(a_name))) {
					Audio::Play(wav, 1.0f);
				}
			} else if (const auto* sound = g->db->sound(a_name)) {
				PlayEntry(*sound, nullptr);
			}
		}

		// Calls the global function a_name with string/integer arguments already pushed after it.
		void Call(lua_State* L, int a_args, const char* a_name)
		{
			if (lua_pcall(L, a_args, 0, 0) != 0) {
				logger::error("wow ui: {}: {}", a_name, lua_tostring(L, -1));
				lua_pop(L, 1);
			}
		}

		bool Run(lua_State* L, const char* a_text, std::size_t a_size, const char* a_name)
		{
			a_size = a_size ? a_size : std::strlen(a_text);
			if (luaL_loadbuffer(L, a_text, a_size, a_name) || lua_pcall(L, 0, 0, 0)) {
				logger::error("wow ui: {}: {}", a_name, lua_tostring(L, -1));
				lua_pop(L, 1);
				return false;
			}
			return true;
		}

		void SaveWtf(lua_State* L)
		{
			for (const auto& [function, file] : { std::pair{ "__WBSavedVariablesText", Wtf("SavedVariables.lua") },
                    std::pair{ "__WBCharacterVariablesText", Wtf("SavedVariables.lua", true) },
					 std::pair{ "__WBLayoutCacheText", LayoutFile() } }) {
				lua_getglobal(L, function);
				if (lua_pcall(L, 0, 1, 0) != 0) {
					logger::error("wow ui: {}: {}", function, lua_tostring(L, -1));
				} else if (const char* text = lua_tostring(L, -1)) {
					WriteText(file, text);
				}
				lua_pop(L, 1);
			}
		}

		bool Start(ID3D11Device* a_device, ID3D11DeviceContext* a_context, int a_width, int a_height,
			const std::string& a_reloadState = {})
		{
			g = new State();
			g->textures.device = a_device, g->textures.context = a_context;
			const auto data = Config::ClientData();
			try {
				// Skyrim's magic has WoW icons of its own (tools/skyrim_icons.py), shipped loose beside the plugin.
				g->mpq = std::make_shared<wowcore::Mpq>(data, std::filesystem::path("Data/SKSE/Plugins/WoWBridge/Icons"),
					std::filesystem::path("Data/SKSE/Plugins/WoWBridge/Minimap"));
			} catch (const std::exception& e) {
				logger::error("wow ui: the WoW 1.12 archives could not be opened at {}: {}", data, e.what());
				return false;
			}
			g->db = std::make_unique<wowcore::ClientDb>(*g->mpq);
			try {
				std::ifstream index("Data/SKSE/Plugins/WoWBridge/Minimap/index.tsv");
				if (index) g->minimap = wowcore::MinimapAtlas::read(index);
				else logger::info("wow ui: no static minimap atlas installed");
			} catch (const std::exception& e) {
				logger::error("wow ui: static minimap atlas: {}", e.what());
			}
			if (!g->db->ok()) {
				logger::error("wow ui: client tables missing from {}", data);
				return false;
			}
			g->ui = std::make_unique<wowcore::UiSession>(g->mpq, *g->db, g->textures, static_cast<float>(a_width), static_cast<float>(a_height));
			std::string err;
			if (!g->ui->start(&err)) {
				logger::error("wow ui: interface engine: {}", err);
				return false;
			}
			ImGuiContext* previous = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(g->ui->imguiContext());
			const bool backend = ImGui_ImplDX11_Init(a_device, a_context);
			ImGui::SetCurrentContext(previous);
			if (!backend) {
				logger::error("wow ui: ImGui DX11 backend init failed");
				return false;
			}
			lua_State* L = g->ui->lua();
			// The interface's commands. Cursor changes are the interface's own business here; the rest (casts, targeting,
			// movement, bindings) go to the gameplay bridge (M7) and are logged until it takes them.
			wowcore::setHostHandler(L, [](lua_State* s, const std::string& a_cmd) -> int {
				if (a_cmd == "setCursor") {
					g->uiCursor = lua_isstring(s, 2) ? lua_tostring(s, 2) : "";
				} else if (a_cmd == "cursor") {
					g->spellCursor = lua_isstring(s, 4) ? lua_tostring(s, 4) : "";
				} else if (a_cmd == "taxi") {
					Taxi::Command(luaL_checkstring(s,2),static_cast<std::uint32_t>(luaL_checknumber(s,3)),
						static_cast<std::uint32_t>(luaL_checknumber(s,4)),static_cast<int>(luaL_checknumber(s,5)));
				} else if (a_cmd == "merchant") {
					Merchant::Command(luaL_checkstring(s,2),luaL_checkstring(s,3),static_cast<int>(luaL_checknumber(s,4)),
						static_cast<std::uint32_t>(luaL_checknumber(s,5)),static_cast<std::uint32_t>(luaL_checknumber(s,6)));
				} else if (a_cmd == "trainer") {
					Trainers::Command(luaL_checkstring(s,2),static_cast<std::uint32_t>(luaL_checknumber(s,3)),
						static_cast<int>(luaL_checknumber(s,4)),static_cast<int>(luaL_checknumber(s,5)),static_cast<int>(luaL_checknumber(s,6)));
				} else if (a_cmd == "bagCursor") {
					Bags::Cursor(luaL_checkstring(s,2),luaL_checkstring(s,3),static_cast<int>(luaL_checknumber(s,4)),
						static_cast<std::uint32_t>(luaL_checknumber(s,5)),luaL_checkstring(s,6),luaL_checkstring(s,7),
						static_cast<std::uint32_t>(luaL_checknumber(s,8)));
				} else if (a_cmd == "bagUse" || a_cmd == "bagUseEffect") {
					Bags::Use(luaL_checkstring(s, 2), a_cmd == "bagUseEffect");
				} else if (a_cmd == "bagPage") {
					Bags::Page(static_cast<std::uint32_t>(luaL_checknumber(s, 2)));
                } else if (a_cmd == "lootDeposit") {
                    Loot::Deposit(static_cast<std::uint32_t>(luaL_checknumber(s, 2)), luaL_checkstring(s, 3), static_cast<int>(luaL_checknumber(s, 4)));
				} else if (a_cmd == "lootTake") {
					Loot::Take(static_cast<std::uint32_t>(luaL_checknumber(s, 2)), luaL_checkstring(s, 3), static_cast<int>(luaL_checknumber(s, 4)));
				} else if (a_cmd == "lootClose") {
					Loot::Close(static_cast<std::uint32_t>(luaL_checknumber(s, 2)));
				} else if (a_cmd == "playSound" || a_cmd == "playSoundFile") {
					if (lua_isstring(s, 2)) {
						PlayWowSound(lua_tostring(s, 2), a_cmd == "playSoundFile");
					}
				} else if (a_cmd == "reloadUI" || a_cmd == "logout" || a_cmd == "quit") {
					Run(s, "__WBFire('PLAYER_LOGOUT')", 0, "=logout");
					SaveWtf(s);
					if (a_cmd == "reloadUI") g->reload = true;   // rebuild after this Lua call has returned
				} else if (a_cmd == "actionChanged") {   // a spell put on or taken off her bar: kept for the next session
					WriteText(ActionsFile(), ActionsLua(s));
				} else if (a_cmd == "barToggles") {   // the extra action bars shown (Interface Options), as the client keeps them
					std::string lua = "__WB.barToggles = {";
					for (int i = 2; i <= 5; ++i) {
						lua += lua_toboolean(s, i) ? " 1," : " nil,";
					}
					WriteText(Wtf(Config::Get().autorun ? "bars-test.lua" : "bars.lua", true), lua + " }");
				} else if (a_cmd == "saveBindings") {
					const bool character = lua_tonumber(s, 2) == 2;
					WriteText(Wtf("bindings-cache.wtf", character), lua_isstring(s, 3) ? lua_tostring(s, 3) : "");
					if (!character) {   // saving the account's set leaves the character's behind, as the client does
						std::error_code ec;
						std::filesystem::remove(Wtf("bindings-cache.wtf", true), ec);
					}
				} else if (a_cmd == "cvar") {
					if (lua_isstring(s, 2)) {
						g_cvars[lua_tostring(s, 2)] = lua_isstring(s, 3) ? lua_tostring(s, 3) : "";
						std::string text;
						for (const auto& [name, value] : g_cvars) {
							text += std::format("SET {} \"{}\"\n", name, value);
						}
						WriteText(Wtf("config-cache.wtf"), text);
					}
				} else if (a_cmd == "useAction" || a_cmd == "castSpell" || a_cmd == "castSpellByName" || a_cmd == "stopCasting" ||
						   a_cmd == "command" || a_cmd == "spellTargetUnit" || a_cmd == "menu" || a_cmd == "stopTargeting" ||
						   a_cmd == "cancelAura" || a_cmd == "shapeshift" || a_cmd == "levelUp" || a_cmd == "learnTalent" || a_cmd == "requestTalents" ||
						   a_cmd == "nameplateTarget" || a_cmd == "testCommand") {
					if (!Integration::Active()) return 0;
					Command c{ a_cmd, {} };
					for (int i = 2; i <= lua_gettop(s); ++i) {
						c.args.emplace_back(lua_isstring(s, i) ? lua_tostring(s, i) : lua_isboolean(s, i) && lua_toboolean(s, i) ? "1" : "");
					}
					std::lock_guard lock(g_exchange);
					g_commands.push_back(std::move(c));
				} else {
					static int logged = 0;
					if (logged++ < 50) {
						logger::info("wow ui: command {} {}", a_cmd, lua_isstring(s, 2) ? lua_tostring(s, 2) : "");
					}
				}
				return 0;
			});
			// The reference character's state until the gameplay bridge writes the live one.
			const auto& state = wowcore::kSliceState[0];
			if (!Run(L, state.text, state.size, "@slice_state.lua") || !Run(L, "__WBFixture('idle')", 0, "=fixture")) {
				return false;
			}
			// Her action bar as she left it, else as the realm keeps it (character_action), not the recorded fixture's.
			if (const auto saved = ColdPersistence::DefaultUi(Config::Get().autorun,g_profileReload,Config::Get().testOnly) ? std::string() : ReadText(ActionsFile()); !saved.empty()) {
				Run(L, saved.c_str(), saved.size(), "=actions");
			} else {
				if (!Character::Get().actions.empty()) {
					const auto bar = Character::ActionsLua();
					Run(L, bar.c_str(), bar.size(), "=actions");
				}
				// The rest of her caster-form book where the realm's bar left room: the main bar's last slots, then the
				// bottom left bar (actions 61-72, shown from the start). She moves them as she likes; that is kept.
				static constexpr std::pair<int, std::uint32_t> kBook[] = { { 8, 25298 }, { 9, 9858 }, { 10, 9885 }, { 11, 9910 },
					{ 12, 22812 }, { 61, 9907 }, { 62, 18658 }, { 63, 29166 }, { 64, 9863 }, { 65, 21850 }, { 66, 20748 },
					{ 67, 2782 }, { 68, 2893 }, { 69, 8946 }, { 70, 9901 } };
				std::string book = "local a = __WB.actions or {} __WB.actions = a local on = {} for _, x in pairs(a) do on[x.spell] = true end";
				for (const auto& [slot, spell] : kBook) {
					book += std::format(" if not a[{0}] and not on[{1}] then a[{0}] = {{ spell = {1} }} end", slot, spell);
				}
				book += " __WBDefaultFormActions()";   // her forms' pages: Attack first, then the form's abilities
				Run(L, book.c_str(), book.size(), "=book");
			}
			// The extra action bars shown: as she left them, else the bottom left one (where the book's rest is).
			if (const auto bars = ColdPersistence::DefaultUi(Config::Get().autorun,g_profileReload,Config::Get().testOnly) ? std::string() : ReadText(Wtf(Config::Get().autorun ? "bars-test.lua" : "bars.lua", true)); !bars.empty()) {
				Run(L, bars.c_str(), bars.size(), "=bars");
			} else {
				Run(L, "__WB.barToggles = { 1 }", 0, "=bars");
			}
			// What the player changed last time: cvars and key bindings before the interface loads.
			const auto cvars = ReadText(Wtf("config-cache.wtf"));
			for (std::size_t at = 0; (at = cvars.find("SET ", at)) != std::string::npos; ++at) {
				const auto sp = cvars.find(' ', at + 4), q1 = cvars.find('"', at), q2 = cvars.find('"', q1 + 1);
				if (sp != std::string::npos && q1 != std::string::npos && q2 != std::string::npos) {
					g_cvars[cvars.substr(at + 4, sp - at - 4)] = cvars.substr(q1 + 1, q2 - q1 - 1);
				}
			}
			lua_getglobal(L, "__WBLoadCVars");
			lua_pushlstring(L, cvars.data(), cvars.size());
			Call(L, 1, "__WBLoadCVars");
			for (const bool character : { true, false }) {
				if (const auto text = ReadText(Wtf("bindings-cache.wtf", character)); !text.empty()) {
					lua_getglobal(L, "__WBLoadBindings");
					lua_pushinteger(L, character ? 2 : 1);
					lua_pushlstring(L, text.data(), text.size());
					Call(L, 2, "__WBLoadBindings");
					break;
				}
			}
			if (!g->ui->loadInterface(&err)) {
				logger::error("wow ui: FrameXML: {}", err);
				return false;
			}
			if (!a_reloadState.empty() && !Run(L, a_reloadState.c_str(), a_reloadState.size(), "=reload-state")) return false;
			// The interface's saved variables, after its own defaults and before VARIABLES_LOADED (the login burst).
			if (const auto saved = ReadText(Wtf("SavedVariables.lua")); !saved.empty()) {
				Run(L, saved.c_str(), saved.size(), "@SavedVariables.lua");
			}
            // Older installations kept the preferred mount in account variables.
            // Keep that migration only for the legacy character, never a new one.
            if (g_renderProfile != UiProfile::Legacy) Run(L, "WBPreferredMount=nil", 0, "=mount-default");
            if (const auto saved = ReadText(Wtf("SavedVariables.lua", true)); !saved.empty())
                Run(L, saved.c_str(), saved.size(), "@CharacterSavedVariables.lua");
			static bool fresh = Config::Get().autorun && !ColdPersistence::VerifyOnly(Config::Get().testOnly);   // a test run starts from the default layout; its reloads keep theirs
			if (std::exchange(fresh, false)) {
				std::error_code ec;
				std::filesystem::remove(LayoutFile(), ec);
			}
			const auto layout = ReadText(LayoutFile());
			lua_getglobal(L, "__WBLoadLayoutCache");
			lua_pushlstring(L, layout.data(), layout.size());
			Call(L, 1, "__WBLoadLayoutCache");
			if (!Run(L, "__WBLogin()", 0, "=login") || !Run(L, "__WBSkyrimMenus()", 0, "=menus")) {
				return false;
			}
			g->last = std::chrono::steady_clock::now();
			++g_generation;
			g_ready = true;
			logger::info("wow ui: 1.12 interface running at {}x{} from {} ({} of 5 interface fonts)", a_width, a_height, data,
				g->ui->fontsLoaded());
			return true;
		}
	}

	void Render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, int a_width, int a_height)
	{
		std::lock_guard session(g_session);
        std::optional<std::string> profile;
        { std::lock_guard lock(g_exchange); profile.swap(g_nextProfile); }
        if (profile && *profile != g_renderProfile) {
            g_profileReload = g != nullptr;
            if (g) {
                if (!g->failed) {
                    // Flush under the OLD key before replacing the Lua session.
                    SaveWtf(g->ui->lua());
                    WriteText(ActionsFile(), ActionsLua(g->ui->lua()));
                    ImGuiContext* previous = ImGui::GetCurrentContext();
                    ImGui::SetCurrentContext(g->ui->imguiContext());
                    ImGui_ImplDX11_Shutdown();
                    ImGui::SetCurrentContext(previous);
                }
                delete g; g = nullptr;
            }
            g_renderProfile = *profile;
            g_lastButtons = {}; g_overUi = false;
        }
		if (g && g->reload && !g->failed) {
			g_ready = false;
			// The old Lua stack is idle here. Release its backend in its own ImGui context,
			// then construct a fresh interface using the same login/WTF path as a new session.
			lua_State* L = g->ui->lua();
			lua_getglobal(L, "__WBReloadStateText");
			if (lua_pcall(L, 0, 1, 0) != 0) {
				logger::error("wow ui: reload state: {}", lua_tostring(L, -1));
				lua_pop(L, 1);
				g->reload = false;
				g_ready = true;
				return;
			}
			const std::string state = lua_tostring(L, -1);
			lua_pop(L, 1);
			const auto clock = g->clock;
			ImGuiContext* previous = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(g->ui->imguiContext());
			ImGui_ImplDX11_Shutdown();
			ImGui::SetCurrentContext(previous);
			delete g;
			g = nullptr;
			if (!Start(a_device, a_context, a_width, a_height, state)) g->failed = true;
			g->clock = clock;
			g_lastButtons = {};
		}
		if (!g) {
			if (!Start(a_device, a_context, a_width, a_height)) {
				g->failed = true;
			}
		}
		if (g->failed) {
			return;
		}
        g_ready = true; // includes a load of the already active profile
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::clamp(std::chrono::duration<float>(now - g->last).count(), 0.0f, 0.25f);
		g->last = now;
		g->clock += dt;
		wowee::core::setAppTimeSeconds(g->clock);

		std::string snapshot;
		{
			std::lock_guard lock(g_exchange);
			snapshot.swap(g_snapshot);
		}
		if (!snapshot.empty()) {
			Run(g->ui->lua(), ("__WBApplyLive(" + snapshot + ")").c_str(), 0, "=live");
		}
		std::vector<std::function<void()>> tasks;
		{
			std::lock_guard lock(g_exchange);
			tasks.swap(g_tasks);
		}
		for (const auto& task : tasks) {
			task();
		}
		// Rendering reads only copied Lua pose/blips; no Skyrim traversal here.
		g->ui->applyLiveMinimap(&g->minimap);
		// CharacterModelFrame uses the very same assembled look as the world renderer.
		// A new session must receive it again, including ReloadUI.
		if (const auto* look=Avatar::Data(); look && look!=g->characterLook) {
			g->ui->setPlayerLook(look->look, look->appearance, look->items); g->characterLook=look;
		}

		// Her face in the player's portraits (a live render, drawn as a disc by the widget renderer).
		if (auto* portrait = WowModels::Portrait()) {
			auto& widgets = g->ui->engine().widgets();
			const auto face = reinterpret_cast<std::uint64_t>(portrait);
			if (auto* w = widgets.findByName("PlayerPortrait")) {
				w->externalTexture = face;
				w->isUnitPortrait = true;
			}
			for (const auto id : widgets.playerPortraits()) {
				if (auto* w = widgets.get(id)) {
					w->externalTexture = face;
				}
			}
		}
		// The target's: a Skyrim actor's face when one is drawn, else what the interface put there.
		{
			auto&      widgets = g->ui->engine().widgets();
			auto*      target = Bridge::Target();
			const auto face = reinterpret_cast<std::uint64_t>(target && target->IsPlayerRef() ? WowModels::Portrait() : WowModels::TargetPortrait());
			if (auto* w = widgets.findByName("TargetPortrait")) {
				w->externalTexture = face;
				w->isUnitPortrait = face != 0;
			}
			for (const auto id : widgets.portraitsFor("target")) {
				if (auto* w = widgets.get(id)) {
					w->externalTexture = face;
				}
			}
		}
		// Party textures are selected against the copied Lua roster, never a Skyrim pointer on this thread.
		{
			auto* L = g->ui->lua();
			lua_getglobal(L, "__WB"); lua_getfield(L, -1, "tokens");
			for (std::size_t i = 1; i <= 4; ++i) {
				const auto token = std::format("party{}", i);
				lua_getfield(L, -1, token.c_str());
				const auto actor = static_cast<RE::FormID>(lua_tonumber(L, -1));
				lua_pop(L, 1);
				const auto face = reinterpret_cast<std::uint64_t>(actor ? WowModels::UnitPortrait(i, actor) : nullptr);
				for (const auto id : g->ui->engine().widgets().portraitsFor(token)) {
					if (auto* w = g->ui->engine().widgets().get(id)) { w->externalTexture = face; w->isUnitPortrait = face != 0; }
				}
			}
			lua_pop(L, 2);
		}
		// Paused native menus stop rendering this HUD entirely. Remember a lost
		// input capture so its gesture is cancelled on the first returning frame.
		static std::uint64_t captureEpoch=0;
		const auto currentCapture=Input::CaptureEpoch();
		if (currentCapture!=captureEpoch) {
			if (g->ui->engine().holdsMousePress()) logger::info("wow ui: cancelled pointer gesture after native input capture changed");
			g->ui->engine().cancelMousePress(); g_lastButtons={}; captureEpoch=currentCapture;
		}
		if (Input::Active()) {
			const auto [x, y] = Input::Cursor();
			const auto b = Input::MouseButtons();
			auto& engine = g->ui->engine();
			// Right-click cancels a carried bag quantity or pending merchant purchase on every surface.
			lua_getglobal(g->ui->lua(), "__WB"); lua_getfield(g->ui->lua(), -1, "bagHeld");
			bool carried=lua_istable(g->ui->lua(),-1); lua_pop(g->ui->lua(),1);
			lua_getfield(g->ui->lua(), -1, "merchantCursor"); carried=carried || lua_istable(g->ui->lua(),-1);
			const bool cancelBag=b.right && !g_lastButtons.right && carried;
			lua_pop(g->ui->lua(),2);
			static bool cancelBagGesture=false;
			if (cancelBag) { cancelBagGesture=true; Run(g->ui->lua(), "ClearCursor()",0,"=bag-cancel"); }
			engine.dispatchMouse(x, y, static_cast<float>(a_height), { b.left, b.right && !cancelBagGesture, b.middle });
			if (!b.right) cancelBagGesture=false;
			g->ui->pointer(x, y);
			// A press no WoW frame took is a click in the world, as the client's: either button's on release, and only
			// when it did not drag meanwhile (a left drag turns the camera, a right drag steers) and the two were not held
			// together (both held run her forward).
			static bool leftInWorld = false, rightInWorld = false, together = false;
			const bool  pressL = b.left && !g_lastButtons.left, pressR = b.right && !g_lastButtons.right;
			const bool  releaseL = !b.left && g_lastButtons.left, releaseR = !b.right && g_lastButtons.right;
			if (pressL) {
				leftInWorld = !engine.mouseOverFrameXml();
				Input::SetLeftLook(leftInWorld);
			}
			if (pressR) {
				rightInWorld = !cancelBag && !engine.mouseOverFrameXml();
			}
			if (b.left && b.right) {
				together = true;
			}
			std::lock_guard lock(g_exchange);
			if (releaseL) {
				lua_getglobal(g->ui->lua(), "__WB");
				lua_getfield(g->ui->lua(), -1, "bagHeld");
				bool bagHeld=lua_istable(g->ui->lua(),-1); lua_pop(g->ui->lua(),1);
				lua_getfield(g->ui->lua(), -1, "merchantCursor"); bagHeld=bagHeld || lua_istable(g->ui->lua(),-1);
				lua_pop(g->ui->lua(),2);
				if (bagHeld ? !engine.mouseOverFrameXml() && !together : leftInWorld && !Input::LeftDragged() && !together) {
					g_clicks.push_back({ x, y, 0, Input::AltHeld() });
				}
				leftInWorld = false;
				Input::SetLeftLook(false);
			}
			if (releaseR && rightInWorld && !Input::RightDragged() && !together) {
				g_clicks.push_back({ x, y, 1, Input::AltHeld() });
			}
			if (!b.left && !b.right) {
				together = false;
			}
			g_lastButtons = b;
		} else {
			// Native menus/focus changes can consume the button-up event. Keeping
			// the old drag would turn the next ordinary click into OnReceiveDrag.
			g->ui->engine().cancelMousePress();
			g_lastButtons = {};
		}
		ImGuiContext* previous = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g->ui->imguiContext());
		ImGui_ImplDX11_NewFrame();
		std::string worldCursor;
		{
			std::lock_guard lock(g_exchange);
			worldCursor = g_worldCursor;
		}
		const bool drawCursor = Input::Active() && !Input::SteeringHeld();
		const auto [cx, cy] = Input::Cursor();
		const bool overUi = g->ui->engine().mouseOverFrameXml();
		g_overUi = overUi;
		const std::string cursor = !g->uiCursor.empty() ? g->uiCursor : overUi ? std::string() : worldCursor;
		auto cursorTexture = [&](const std::string& a_file) -> ImTextureID {
			if (auto it = g->cursors.find(a_file); it != g->cursors.end()) {
				return it->second;
			}
			ImTextureID id = ImTextureID_Invalid;
			if (auto bytes = g->mpq->read(a_file)) {
				const auto img = wowee::pipeline::BLPLoader::load(*bytes);
				if (img.isValid()) {
					id = reinterpret_cast<ImTextureID>(g->textures.uploadImGuiTexture(img.data.data(), img.width, img.height));
				}
			}
			return g->cursors[a_file] = id;
		};
		ImDrawData* dd = g->ui->frame(dt, [&](ImDrawList* a_list) {
			if (!drawCursor) {
				return;
			}
			std::string file = g->spellCursor.empty() ? CursorFile(cursor.empty() ? "POINT" : cursor) : g->spellCursor;
			if (!file.ends_with(".blp")) file += ".blp";
			const ImTextureID tex = cursorTexture(file);
			if (tex != ImTextureID_Invalid) {
				a_list->AddImage(tex, ImVec2(cx, cy), ImVec2(cx + 32.0f, cy + 32.0f));
			}
		});
		ImGui::SetCurrentContext(g->ui->imguiContext());
		ImGui_ImplDX11_RenderDrawData(dd);
		ImGui::SetCurrentContext(previous);
	}

	bool Ready() { return g_ready && Integration::Active(); }

	void ClearPending()
	{
		std::lock_guard lock(g_exchange);
		g_tasks.clear(); g_commands.clear(); g_clicks.clear(); g_snapshot.clear(); g_worldCursor.clear();
	}

	void Publish(std::string a_snapshot)
	{
		std::lock_guard lock(g_exchange);
		g_snapshot = std::move(a_snapshot);
	}

	void Run(std::string a_lua)
	{
		std::lock_guard lock(g_exchange);
		g_tasks.push_back([lua = std::move(a_lua)] { Run(g->ui->lua(), lua.c_str(), 0, "=host"); });
	}

	std::filesystem::path WtfFile(std::string_view a_file, bool a_character)
    { std::lock_guard session(g_session); return Wtf(a_file, a_character); }

    std::uint64_t Generation() { return g_generation.load(); }
    std::string ProfileKey() { return g_profile.key; }
    void NewGame() { g_profile.key = NewProfileKey(); RequestProfile(); }
    void PrepareLoad() { g_profile.LegacyLoad(); }
    void OnGameLoaded() { RequestProfile(); }
    bool SelectProfileForTesting(std::string_view key)
    {
        if (!g_profile.Load(kProfileVersion, key)) return false;
        RequestProfile(); return true;
    }
    void SaveProfile(SKSE::SerializationInterface* intfc)
    {
        if (!intfc->OpenRecord(kProfileRecord, kProfileVersion) ||
            !intfc->WriteRecordData(g_profile.key.data(), static_cast<std::uint32_t>(g_profile.key.size())))
            logger::error("wow ui: character profile was not saved");
    }
    void LoadProfile(SKSE::SerializationInterface* intfc, std::uint32_t length, std::uint32_t version)
    {
        std::string key;
        if (length <= UiProfile::MaxLength && length > 0) {
            key.resize(length);
            if (intfc->ReadRecordData(key.data(), length) == length && g_profile.Load(version, key)) return;
        }
        g_profile.key = NewProfileKey();
        logger::warn("wow ui: invalid character profile; using a fresh isolated profile");
    }

	void SaveVariables()
	{
		std::lock_guard session(g_session);
		if (!Ready()) {
			return;
		}
		SaveWtf(g->ui->lua());
	}

	KeyRoute Route(const std::string& a_key, const std::string& a_bareKey)
	{
		std::lock_guard session(g_session);
		if (!Ready()) {
			return {};
		}
		auto& engine = g->ui->engine();
		if (engine.editBoxHasFocus()) {
			return { KeyRoute::To::Edit, {} };
		}
		if (engine.keyboardFrameShown()) {
			return { KeyRoute::To::Frame, {} };
		}
		auto command = engine.bindingCommandForKey(a_key);
		// Shift-B's former all-bag shortcut is deliberately unbound; it must
		// not fall through to B after the Lua adapter removes that binding.
		if (command.empty() && a_key != a_bareKey && a_key != "SHIFT-B") {
			command = engine.bindingCommandForKey(a_bareKey);
		}
		return { KeyRoute::To::Binding, std::move(command) };
	}

	void Key(KeyRoute::To a_to, std::string a_key, bool a_down)
	{
		std::lock_guard lock(g_exchange);
		g_tasks.push_back([a_to, key = std::move(a_key), a_down] {
			auto& engine = g->ui->engine();
			switch (a_to) {
			case KeyRoute::To::Binding:
				Run(g->ui->lua(), std::format("__WBBinding(\"{}\", {})", key, a_down ? "true" : "false").c_str(), 0, "=binding");
				break;
			case KeyRoute::To::Frame:
				engine.dispatchFrameKeyName(key, a_down);
				break;
			case KeyRoute::To::Edit:
				// An edit box reads the editing keys; what they type arrives as text.
				static const std::map<std::string, int> kEditKeys = { { "ENTER", '\r' }, { "ESCAPE", 27 }, { "BACKSPACE", '\b' },
					{ "TAB", '\t' }, { "HOME", 0x4000004A }, { "END", 0x4000004D }, { "DELETE", 0x4000004C },
					{ "LEFT", 0x40000050 }, { "RIGHT", 0x4000004F }, { "UP", 0x40000052 }, { "DOWN", 0x40000051 } };
				if (const auto it = kEditKeys.find(key); a_down && it != kEditKeys.end()) {
					engine.dispatchKey(it->second, false);
				}
				break;
			}
		});
	}

	void Text(std::string a_utf8)
	{
		std::lock_guard lock(g_exchange);
		g_tasks.push_back([text = std::move(a_utf8)] { g->ui->engine().dispatchText(text.c_str()); });
	}

	std::vector<Command> TakeCommands()
	{
		std::lock_guard lock(g_exchange);
		return std::exchange(g_commands, {});
	}

	SpellFacts Facts(std::uint32_t a_spell)
	{
		std::lock_guard session(g_session);
		SpellFacts f;
		const auto* s = Ready() ? g->db->spell(a_spell) : nullptr;
		if (!s) {
			return f;
		}
		f.known = true;
		f.name = s->name;
		f.rangeMax = s->rangeMax;
		f.speed = s->speed;
		for (const auto& e : s->effects) {
			f.radius = std::max(f.radius, e.radius);
			f.resurrect = f.resurrect || e.effect == 113; // SPELL_EFFECT_RESURRECT_NEW
		}
		// ImplicitTargetA: 6 single enemy, 28/53/16 places and areas at a place; friends, self and the rest are not harmful.
		f.harmful = s->implicitTarget == 6;
		f.ground = s->implicitTarget == 28 || s->implicitTarget == 53 || s->implicitTarget == 16;
		f.self = s->implicitTarget == 1 || s->implicitTarget == 22;   // the caster, or around her
		return f;
	}

	SpellSounds Sounds(std::uint32_t a_spell)
	{
		std::lock_guard session(g_session);
		if (!Ready()) {
			return {};
		}
		const auto s = g->db->spellSounds(a_spell);
		return { s.precast, s.cast, s.impact, s.state, s.channel, s.precastAnim, s.castAnim, s.channelAnim };
	}

	std::uint32_t SwingSound(std::uint32_t a_swingType, bool a_crit)
	{
		std::lock_guard session(g_session);
		return Ready() ? g->db->weaponSwingSound(a_swingType, a_crit) : 0;
	}

	std::uint32_t ImpactSound(std::uint32_t a_subclass, std::uint32_t a_parryType, std::uint32_t a_index, bool a_crit)
	{
		std::lock_guard session(g_session);
		return Ready() ? g->db->weaponImpactSound(a_subclass, a_parryType, a_index, a_crit) : 0;
	}

	MeleeSounds Melee(Striker a_by, std::uint32_t a_id, std::uint32_t a_parryType, std::uint32_t a_index, bool a_crit)
	{
		std::lock_guard session(g_session);
		const auto s = Ready() ? g->db->meleeSounds(static_cast<wowcore::MeleeSoundSource>(a_by), a_id, a_parryType, a_index, a_crit) : std::nullopt;
		return s ? MeleeSounds{ s->swing, s->impact, s->exertion } : MeleeSounds{};
	}

	std::uint64_t PlaySound(std::uint32_t a_soundId, const RE::NiPoint3* a_at)
	{
		std::lock_guard session(g_session);
		const auto* sound = a_soundId && Ready() ? g->db->sound(a_soundId) : nullptr;
		return sound ? PlayEntry(*sound, a_at) : 0;
	}

	void SetWorldCursor(std::string a_name)
	{
		std::lock_guard lock(g_exchange);
		g_worldCursor = std::move(a_name);
	}

	bool PointerOverInterface() { return g_overUi; }

	std::vector<Click> TakeWorldClicks()
	{
		std::lock_guard lock(g_exchange);
		return std::exchange(g_clicks, {});
	}

	std::string Eval(const std::string& a_lua)
	{
		std::lock_guard session(g_session);
		if (!Ready()) {
			return "(wow ui not running)";
		}
		lua_State* L = g->ui->lua();
		const int top = lua_gettop(L);
		std::string out;
		if (luaL_dostring(L, ("return " + a_lua).c_str())) {
			out = std::string("error: ") + lua_tostring(L, -1);
		} else if (lua_gettop(L) > top) {
			out = lua_isstring(L, top + 1) ? lua_tostring(L, top + 1) : luaL_typename(L, top + 1);
		}
		lua_settop(L, top);
		return out;
	}

	void ProbeDrag(std::string a_source, std::string a_target)
	{
		std::lock_guard lock(g_exchange);
		g_tasks.push_back([source=std::move(a_source),target=std::move(a_target)] {
			if (!Ready()) return;
			auto& engine=g->ui->engine();
			if (engine.holdsMousePress()) { logger::warn("selftest: drag probe refused an existing mouse press"); return; }
			const auto& tree=engine.widgets();
			const float scale=tree.uiScale(), height=static_cast<float>(g->ui->height());
			std::pair<float,float> to{};
			if (target.empty()) {
				bool found=false;
				for (int row=1;row<8 && !found;++row) for (int col=1;col<16 && !found;++col) {
					const float x=g->ui->width()*col/16.f,y=height*row/8.f;
					if (!tree.hitTest(x/scale,(height-y)/scale)) { to={x,y}; found=true; }
				}
				if (!found) { logger::warn("selftest: drag probe found no world surface"); return; }
			} else {
				const auto* w=tree.findByName(target);
				if (!w || !w->visible) return;
				to={(w->left+w->rectW/2)*scale,height-(w->bottom+w->rectH/2)*scale};
			}
			auto from=to;
			if (!source.empty()) {
				const auto* w=tree.findByName(source);
				if (!w || !w->visible) return;
				from={(w->left+w->rectW/2)*scale,height-(w->bottom+w->rectH/2)*scale};
			}
			engine.dispatchMouse(from.first,from.second,height,{});
			engine.dispatchMouse(from.first,from.second,height,{true,false,false});
			if (!source.empty()) engine.dispatchMouse(from.first+8,from.second,height,{true,false,false});
			engine.dispatchMouse(to.first,to.second,height,{true,false,false});
			engine.dispatchMouse(to.first,to.second,height,{});
			// Match Render's inventory-release route: Bridge handles it on the
			// main thread, before picking a world reference.
			if (target.empty() && !engine.mouseOverFrameXml()) {
				lua_State* L=g->ui->lua();
				lua_getglobal(L,"__WBBagCursorHeld");
				const bool carried=lua_pcall(L,0,1,0)==0 && lua_toboolean(L,-1);
				lua_pop(L,1);
				if (carried) { std::lock_guard exchange(g_exchange); g_clicks.push_back({to.first,to.second,0,false}); }
			}
		});
	}
}
