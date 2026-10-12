#pragma once

// WoW's sounds (16-bit PCM WAV, as every interface and spell sound in the 1.12 client is) through XAudio2, fire and
// forget. Scaled by Skyrim's master volume: the WoW sound options open Skyrim's (__WBSkyrimMenus).
namespace Audio
{
	using Wav = std::shared_ptr<const std::vector<std::uint8_t>>;

	// A sound in the world: heard in full within minYards of you, fading as minYards / distance out to cutoffYards,
	// panned by where it is from the camera's facing.
	struct Where
	{
		RE::NiPoint3 at;
		float        minYards = 8.0f, cutoffYards = 40.0f;
	};

	// Returns a handle to Stop a loop with (0 when nothing plays).
	std::uint64_t Play(Wav a_wav, float a_volume, bool a_loop = false, const Where* a_where = nullptr);
	void          Stop(std::uint64_t a_handle);
	std::uint64_t Started();   // voices started (tests)
}
