#include "Audio.h"

// XAudio2 2.9 is loaded at runtime (xaudio2_9.dll, Windows 10's - FAudio under Proton); its header wants a Windows 8+
// target, which the rest of the plugin does not set.
#if _WIN32_WINNT < 0x0602
#	undef _WIN32_WINNT
#	define _WIN32_WINNT 0x0A00
#endif
#include <xaudio2.h>

namespace Audio
{
	namespace
	{
		struct Playing
		{
			IXAudio2SourceVoice* voice = nullptr;
			Wav                  wav;   // the samples, alive while the voice plays them
			std::uint64_t        handle = 0;
		};

		std::mutex             g_lock;
		IXAudio2*              g_xaudio = nullptr;
		IXAudio2MasteringVoice* g_master = nullptr;
		UINT32                 g_masterChannels = 2;
		bool                   g_tried = false;
		std::vector<Playing>   g_playing;
		std::uint64_t          g_started = 0;

		bool Ready()
		{
			if (g_tried) {
				return g_xaudio != nullptr;
			}
			g_tried = true;
			HMODULE dll = ::LoadLibraryW(L"xaudio2_9.dll");
			using Create = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
			auto create = dll ? reinterpret_cast<Create>(::GetProcAddress(dll, "XAudio2Create")) : nullptr;
			if (!create || FAILED(create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(g_xaudio->CreateMasteringVoice(&g_master))) {
				logger::error("audio: XAudio2 unavailable; WoW sounds are silent");
				g_xaudio = nullptr;
				return false;
			}
			XAUDIO2_VOICE_DETAILS details{};
			g_master->GetVoiceDetails(&details);
			g_masterChannels = details.InputChannels;
			logger::info("audio: XAudio2 ready ({} channels)", g_masterChannels);
			return true;
		}

		void Reap()
		{
			std::erase_if(g_playing, [](const Playing& p) {
				XAUDIO2_VOICE_STATE state{};
				p.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
				if (state.BuffersQueued == 0) {
					p.voice->DestroyVoice();
					return true;
				}
				return false;
			});
		}

		float SkyrimMasterVolume()
		{
			auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
			auto* setting = prefs ? prefs->GetSetting("fAudioMasterVolume:AudioMenu") : nullptr;
			return setting ? std::clamp(setting->GetFloat(), 0.0f, 1.0f) : 1.0f;
		}
	}

	std::uint64_t Play(Wav a_wav, float a_volume, bool a_loop, const Where* a_where)
	{
		// RIFF/WAVE: the "fmt " chunk is the voice's format, the "data" chunk its samples.
		const auto& d = *a_wav;
		if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
			return 0;
		}
		WAVEFORMATEX         format{};
		const std::uint8_t*  samples = nullptr;
		std::uint32_t        bytes = 0;
		for (std::size_t at = 12; at + 8 <= d.size();) {
			std::uint32_t size = 0;
			std::memcpy(&size, d.data() + at + 4, 4);
			const auto* body = d.data() + at + 8;
			if (std::memcmp(d.data() + at, "fmt ", 4) == 0 && size >= 16) {
				std::memcpy(&format, body, 16);
			} else if (std::memcmp(d.data() + at, "data", 4) == 0) {
				samples = body;
				bytes = static_cast<std::uint32_t>(std::min<std::size_t>(size, d.size() - (at + 8)));
			}
			at += 8 + size + (size & 1);
		}
		if (!samples || format.wFormatTag != WAVE_FORMAT_PCM || !format.nChannels || format.wBitsPerSample % 8) {
			return 0;
		}
		// Some of the client's files misstate their frame size (iQuestUpdate.wav: stereo 16-bit, block 2): WoW's mixer
		// goes by channels and bits, XAudio2 by the block and reads past the samples - noise, then a fault.
		format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
		bytes -= bytes % format.nBlockAlign;
		// Where it is: how loud for its distance from you, and which side of the camera.
		float gain = 1.0f, pan = 0.0f;
		if (a_where) {
			auto*       player = RE::PlayerCharacter::GetSingleton();
			const auto  to = a_where->at - player->GetPosition();
			const float yards = to.Length() / 64.0f;
			if (yards >= a_where->cutoffYards) {
				return 0;
			}
			gain = yards <= a_where->minYards ? 1.0f : a_where->minYards / yards;
			const auto* camera = RE::PlayerCamera::GetSingleton();
			if (camera && camera->cameraRoot && (to.x != 0.0f || to.y != 0.0f)) {
				const auto& m = camera->cameraRoot->world.rotate;
				const float fx = m.entry[0][0], fy = m.entry[1][0];   // the camera's forward, flattened
				const float len = std::hypot(to.x, to.y), flen = std::max(std::hypot(fx, fy), 1e-4f);
				pan = std::clamp((fx * to.y - fy * to.x) / (flen * len) * -1.0f, -1.0f, 1.0f);   // + right
			}
		}
		std::lock_guard lock(g_lock);
		if (!Ready()) {
			return 0;
		}
		Reap();
		IXAudio2SourceVoice* voice = nullptr;
		if (FAILED(g_xaudio->CreateSourceVoice(&voice, &format))) {
			return 0;
		}
		voice->SetVolume(a_volume * gain * SkyrimMasterVolume());
		if (a_where && format.nChannels == 1 && g_masterChannels >= 2) {
			const float angle = (pan + 1.0f) * 0.25f * RE::NI_PI;   // equal power
			std::vector<float> matrix(g_masterChannels, 0.0f);
			matrix[0] = std::cos(angle), matrix[1] = std::sin(angle);
			voice->SetOutputMatrix(g_master, 1, g_masterChannels, matrix.data());
		}
		XAUDIO2_BUFFER buffer{};
		buffer.Flags = XAUDIO2_END_OF_STREAM;
		buffer.AudioBytes = bytes;
		buffer.pAudioData = samples;
		buffer.LoopCount = a_loop ? XAUDIO2_LOOP_INFINITE : 0;
		if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
			voice->DestroyVoice();
			return 0;
		}
		static std::uint64_t next = 1;
		g_playing.push_back({ voice, std::move(a_wav), next });
		++g_started;
		return next++;
	}

	void Stop(std::uint64_t a_handle)
	{
		std::lock_guard lock(g_lock);
		std::erase_if(g_playing, [&](const Playing& p) {
			if (p.handle != a_handle) {
				return false;
			}
			p.voice->Stop();
			p.voice->DestroyVoice();
			return true;
		});
	}

	std::uint64_t Started()
	{
		std::lock_guard lock(g_lock);
		return g_started;
	}
}
