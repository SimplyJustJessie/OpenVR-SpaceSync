// SPDX-License-Identifier: AGPL-3.0-only
// Added by Shinyflvres, 2026-08-25. Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#include "Sound.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#else
// Linux plays through SDL3 audio (PipeWire/Pulse/ALSA) in place of PlaySound.
#include <SDL3/SDL.h>
#include <chrono>
#endif

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sound
{
	namespace
	{
		const double kVolume = 0.5;
		const double kTailSeconds = 0.1;
		const double kSilence = 0.02;

		std::mutex mutex;
		std::condition_variable wake;
		std::deque<std::pair<std::string, uint64_t>> queue;
		uint64_t generation = 0;
		std::map<std::string, std::vector<uint8_t>> cache;
		std::thread worker;
		bool running = false;
		std::string soundDir;

#ifdef _WIN32
		std::string ExeDirectory()
		{
			char path[MAX_PATH] = {};
			DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
			std::string s(path, n);
			size_t slash = s.find_last_of("\\/");
			return slash == std::string::npos ? "." : s.substr(0, slash);
		}

		const char* kPathSep = "\\";
#else
		std::string ExeDirectory()
		{
			std::string s = SDL_GetBasePath() ? SDL_GetBasePath() : "./";
			if (!s.empty() && s.back() == '/')
				s.pop_back();
			return s;
		}

		const char* kPathSep = "/";

		SDL_AudioStream* stream = nullptr;
		SDL_AudioSpec streamSpec = {};

		// True while the cue queued under gen should keep playing.
		bool StillCurrent(uint64_t gen)
		{
			std::lock_guard<std::mutex> lock(mutex);
			return running && gen == generation;
		}

		// Blocks until the clip finishes or is superseded, matching the
		// SND_SYNC PlaySound call on Windows.
		void PlayBlocking(const std::vector<uint8_t>& wav, uint64_t gen)
		{
			SDL_AudioSpec spec;
			Uint8* pcm = nullptr;
			Uint32 pcmLen = 0;
			if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(wav.data(), wav.size()), true, &spec, &pcm, &pcmLen))
				return;

			if (stream && (spec.format != streamSpec.format || spec.channels != streamSpec.channels || spec.freq != streamSpec.freq))
			{
				SDL_DestroyAudioStream(stream);
				stream = nullptr;
			}
			if (!stream)
			{
				stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
				if (!stream)
				{
					SDL_free(pcm);
					return;
				}
				streamSpec = spec;
				SDL_ResumeAudioStreamDevice(stream);
			}

			SDL_PutAudioStreamData(stream, pcm, (int)pcmLen);
			SDL_FlushAudioStream(stream);
			SDL_free(pcm);

			while (SDL_GetAudioStreamQueued(stream) > 0)
			{
				if (!StillCurrent(gen))
				{
					SDL_ClearAudioStream(stream);
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
#endif

		uint32_t ReadU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
		uint16_t ReadU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

		void WriteU32(uint8_t* p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }

		void PrepareWav(std::vector<uint8_t>& wav)
		{
			if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
				return;
			size_t pos = 12;
			uint16_t format = 0, bits = 0, channels = 1;
			uint32_t rate = 44100;
			while (pos + 8 <= wav.size())
			{
				uint32_t size = ReadU32(wav.data() + pos + 4);
				const uint8_t* body = wav.data() + pos + 8;
				if (pos + 8 + size > wav.size())
					size = (uint32_t)(wav.size() - pos - 8);
				if (std::memcmp(wav.data() + pos, "fmt ", 4) == 0 && size >= 16)
				{
					format = ReadU16(body);
					channels = ReadU16(body + 2);
					rate = ReadU32(body + 4);
					bits = ReadU16(body + 14);
				}
				else if (std::memcmp(wav.data() + pos, "data", 4) == 0)
				{
					if (format == 1 && bits == 16 && channels > 0)
					{
						int16_t* samples = reinterpret_cast<int16_t*>(wav.data() + pos + 8);
						size_t count = size / 2;
						int peak = 0;
						for (size_t i = 0; i < count; i++)
							if (std::abs((int)samples[i]) > peak) peak = std::abs((int)samples[i]);
						size_t last = 0;
						int threshold = (int)(peak * kSilence);
						for (size_t i = 0; i < count; i++)
							if (std::abs((int)samples[i]) > threshold) last = i;
						size_t keep = last + 1 + (size_t)(kTailSeconds * rate) * channels;
						keep -= keep % channels;
						if (keep < count)
						{
							size = (uint32_t)(keep * 2);
							WriteU32(wav.data() + pos + 4, size);
							wav.resize(pos + 8 + size + (size & 1));
							WriteU32(wav.data() + 4, (uint32_t)(wav.size() - 8));
						}
						for (size_t i = 0; i < keep && i < count; i++)
							samples[i] = (int16_t)(samples[i] * kVolume);
					}
					return;
				}
				pos += 8 + size + (size & 1);
			}
		}

		const std::vector<uint8_t>* Load(const std::string& name)
		{
			auto it = cache.find(name);
			if (it != cache.end())
				return it->second.empty() ? nullptr : &it->second;

			std::vector<uint8_t>& buffer = cache[name];
			std::ifstream file(soundDir + kPathSep + "sound" + kPathSep + name + ".wav", std::ios::binary);
			if (file)
			{
				buffer.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
				PrepareWav(buffer);
			}
			return buffer.empty() ? nullptr : &buffer;
		}

		void WorkerLoop()
		{
			for (;;)
			{
				std::string name;
				uint64_t gen = 0;
				{
					std::unique_lock<std::mutex> lock(mutex);
					wake.wait(lock, [] { return !running || !queue.empty(); });
					if (!running)
						return;
					name = queue.front().first;
					gen = queue.front().second;
					queue.pop_front();
				}

				const std::vector<uint8_t>* wav = Load(name);
				if (!wav)
					continue;

				// Stop() between the pop above and here would otherwise let a superseded cue play
				// and push everything after it a step out of sync.
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (!running || gen != generation)
						continue;
				}

#ifdef _WIN32
				PlaySoundA(reinterpret_cast<LPCSTR>(wav->data()), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
#else
				PlayBlocking(*wav, gen);
#endif
			}
		}
	}

	void Init()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (running)
			return;
		soundDir = ExeDirectory();
#ifndef _WIN32
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
			return;
#endif
		running = true;
		worker = std::thread(WorkerLoop);
	}

	void Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!running)
				return;
			running = false;
			queue.clear();
		}
		wake.notify_all();
#ifdef _WIN32
		PlaySoundA(nullptr, nullptr, 0);
#endif
		if (worker.joinable())
			worker.join();
#ifndef _WIN32
		// The worker has exited, so the stream is ours to free.
		if (stream)
		{
			SDL_DestroyAudioStream(stream);
			stream = nullptr;
		}
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
	}

	void Play(const char* name)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!running)
				return;
			queue.push_back({ name, generation });
		}
		wake.notify_one();
	}

	void ClearQueue()
	{
		std::lock_guard<std::mutex> lock(mutex);
		queue.clear();
		generation++;
	}

	void Stop()
	{
		ClearQueue();
#ifdef _WIN32
		PlaySoundA(nullptr, nullptr, 0);
#endif
		// Linux: the generation bump in ClearQueue() makes PlayBlocking() cut the clip.
	}
}
