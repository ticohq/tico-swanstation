/// @file TicoAudio.h
/// @brief Reusable audio manager for Tico libretro frontend
/// Uses callback-based audio with ring buffer and resampling

#pragma once

#include <SDL.h>
#include <SDL_mixer.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include "TicoConfig.h"
#include "TicoLogger.h"

/// Thread-safe lock-free ring buffer for audio samples
template <typename T>
class TicoRingBuffer
{
public:
    explicit TicoRingBuffer(size_t size) : m_buffer(size), m_head(0), m_tail(0) {}

    void Write(const T *data, size_t count)
    {
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t currentHead = m_head.load(std::memory_order_acquire);
        size_t capacity = m_buffer.size();

        size_t freeSpace = (capacity + currentHead - currentTail - 1) % capacity;
        size_t toWrite = (count < freeSpace) ? count : freeSpace;

        if (toWrite == 0)
            return;

        size_t part1 = std::min(toWrite, capacity - currentTail);
        std::copy(data, data + part1, m_buffer.begin() + currentTail);

        if (part1 < toWrite)
        {
            std::copy(data + part1, data + toWrite, m_buffer.begin());
        }

        m_tail.store((currentTail + toWrite) % capacity, std::memory_order_release);
    }

    size_t Read(T *data, size_t count)
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_acquire);
        size_t capacity = m_buffer.size();

        size_t available = (capacity + currentTail - currentHead) % capacity;
        size_t toRead = (count < available) ? count : available;

        if (toRead == 0)
            return 0;

        size_t part1 = std::min(toRead, capacity - currentHead);
        std::copy(m_buffer.begin() + currentHead,
                  m_buffer.begin() + currentHead + part1, data);

        if (part1 < toRead)
        {
            size_t part2 = toRead - part1;
            std::copy(m_buffer.begin(), m_buffer.begin() + part2, data + part1);
        }

        m_head.store((currentHead + toRead) % capacity, std::memory_order_release);
        return toRead;
    }

    size_t Available() const
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t capacity = m_buffer.size();
        return (capacity + currentTail - currentHead) % capacity;
    }

    size_t GetAvailableWrite() const
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t capacity = m_buffer.size();
        size_t used = (capacity + currentTail - currentHead) % capacity;
        return (capacity - 1) - used;
    }

    void Clear()
    {
        m_head.store(0, std::memory_order_release);
        m_tail.store(0, std::memory_order_release);
    }

private:
    std::vector<T> m_buffer;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};

/// Audio manager for libretro cores
/// Uses Mix_HookMusic for callback-based audio with natural backpressure
class TicoAudio
{
public:
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int CHANNELS = 2;
    static constexpr size_t BUFFER_SIZE = SAMPLE_RATE * 4; // ~4 seconds buffer (RingBuffer capacity)
    static constexpr size_t MAX_BUFFERED_SAMPLES = (SAMPLE_RATE * 50 / 1000) * CHANNELS; // 50ms
    static constexpr size_t QUEUE_MAX_BYTES = MAX_BUFFERED_SAMPLES * sizeof(int16_t);

    TicoAudio() : m_buffer(BUFFER_SIZE), m_resampler(nullptr), m_deviceId(0),
                  m_initialized(false), m_paused(false), m_coreSampleRate(SAMPLE_RATE) {}

    ~TicoAudio()
    {
        Shutdown();
    }

    /// Initialize audio system
    bool Init(SDL_AudioDeviceID deviceId = 0)
    {
        if (m_initialized)
            return true;

        s_instance = this;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            if (deviceId == 0)
            {
                LOG_ERROR("AUDIO", "SDL_QueueAudio mode requires a valid device ID");
                return false;
            }
            m_deviceId = deviceId;
            SDL_PauseAudioDevice(m_deviceId, 0); // Unpause
            LOG_AUDIO("Initialized with SDL_QueueAudio (Push)");
        }
        else
        {
            // SDL_mixer should already be initialized by the app
            // Hook our audio callback
            Mix_HookMusic(AudioCallback, this);
            LOG_AUDIO("Initialized with callback-based audio");
        }

        m_initialized = true;
        return true;
    }

    /// Shutdown audio system
    void Shutdown()
    {
        if (!m_initialized)
            return;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            SDL_PauseAudioDevice(m_deviceId, 1);
            SDL_CloseAudioDevice(m_deviceId);
            m_deviceId = 0;
        }
        else
        {
            Mix_HookMusic(nullptr, nullptr);
        }

        if (m_resampler)
        {
            SDL_FreeAudioStream(m_resampler);
            m_resampler = nullptr;
        }

        if (s_instance == this)
            s_instance = nullptr;

        m_initialized = false;
        LOG_AUDIO("Shutdown complete");
    }

    /// Set the core's sample rate for resampling
    void SetCoreSampleRate(double sampleRate)
    {
        if (sampleRate <= 0)
            sampleRate = SAMPLE_RATE;

        int coreSR = static_cast<int>(sampleRate);
        if (coreSR == m_coreSampleRate && m_resampler != nullptr)
        {
            return; // No change needed
        }

        m_coreSampleRate = coreSR;

        // Recreate resampler if sample rates differ
        if (m_resampler)
        {
            SDL_FreeAudioStream(m_resampler);
            m_resampler = nullptr;
        }

        if (coreSR != SAMPLE_RATE)
        {
            m_resampler = SDL_NewAudioStream(
                AUDIO_S16, CHANNELS, coreSR,
                AUDIO_S16, CHANNELS, SAMPLE_RATE);
            if (m_resampler)
            {
                LOG_AUDIO("Resampler created: %d -> %d Hz", coreSR, SAMPLE_RATE);
            }
        }
    }

    /// Push a single audio sample (left, right)
    void PushSample(int16_t left, int16_t right)
    {
        if (!m_initialized)
            return;

        if (m_fastForward)
            return; // fast forward plays no sound

        int16_t samples[2] = {left, right};
        MixTrophy(samples, 2);

        // Never stall the emulation thread: vsync paces the frames, so a
        // sample past the latency cap is dropped.
        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            if (SDL_GetQueuedAudioSize(m_deviceId) >= QUEUE_MAX_BYTES)
                return;
            SDL_QueueAudio(m_deviceId, samples, sizeof(samples));
        }
        else
        {
            if (m_buffer.Available() >= MAX_BUFFERED_SAMPLES)
                return;
            m_buffer.Write(samples, 2);
        }
    }

    /// Push a batch of audio samples
    size_t PushSamples(const int16_t *data, size_t frames)
    {
        if (!m_initialized || !data || frames == 0)
            return 0;

        if (m_fastForward)
            return frames;

        size_t samplesNeeded = frames * CHANNELS;
        const bool mixTrophy = m_trophyActive.load(std::memory_order_relaxed);
        if (mixTrophy)
        {
            m_mixScratch.assign(data, data + samplesNeeded);
            MixTrophy(m_mixScratch.data(), samplesNeeded);
            data = m_mixScratch.data();
        }

        // drop the batch at the latency cap rather than stall (see PushSample)
        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            if (SDL_GetQueuedAudioSize(m_deviceId) >= QUEUE_MAX_BYTES)
                return frames;
            SDL_QueueAudio(m_deviceId, data, samplesNeeded * sizeof(int16_t));
        }
        else
        {
            if (m_buffer.Available() >= MAX_BUFFERED_SAMPLES)
                return frames;
            m_buffer.Write(data, samplesNeeded);
        }
        return frames;
    }

    /// Flush/clear the audio buffer (for state loading, etc.)
    /// Flush/clear the audio buffer (for state loading, etc.)
    void Flush()
    {
        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            SDL_ClearQueuedAudio(m_deviceId);
        }
        else
        {
            m_buffer.Clear();
            if (m_resampler)
            {
                SDL_AudioStreamClear(m_resampler);
            }
        }
        LOG_AUDIO("Buffer flushed");
    }

    /// Pause/unpause
    void SetPaused(bool paused) { m_paused = paused; }
    bool IsPaused() const { return m_paused; }

    /// Fast forward drops the game's audio instead of letting it fall behind.
    void SetFastForward(bool ff) { m_fastForward = ff; }
    bool IsFastForwarding() const { return m_fastForward; }

    //--------------------------------------------------------------------------
    // RetroAchievements trophy SFX
    //
    // The Switch can't open a second audio device, and the SDL_mixer pull
    // callback starves under a busy frame, so we mix a short one-shot chime
    // into the same SDL_QueueAudio stream as the game. The WAV must already be
    // SAMPLE_RATE / CHANNELS / S16 (assets/trophy.wav is converted to match).
    //--------------------------------------------------------------------------

    /// Decode a WAV file (device-free) into the trophy PCM buffer.
    bool LoadTrophy(const char *path)
    {
        SDL_AudioSpec spec;
        Uint8 *buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV(path, &spec, &buf, &len))
        {
            LOG_ERROR("AUDIO", "Trophy SDL_LoadWAV failed: %s", SDL_GetError());
            return false;
        }

        SDL_AudioCVT cvt;
        int need = SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                                     AUDIO_S16SYS, CHANNELS, SAMPLE_RATE);
        if (need < 0)
        {
            LOG_ERROR("AUDIO", "Trophy SDL_BuildAudioCVT failed: %s", SDL_GetError());
            SDL_FreeWAV(buf);
            return false;
        }

        if (need == 0)
        {
            m_trophyPCM.assign(reinterpret_cast<int16_t *>(buf),
                               reinterpret_cast<int16_t *>(buf + len));
        }
        else
        {
            cvt.len = static_cast<int>(len);
            cvt.buf = static_cast<Uint8 *>(SDL_malloc(static_cast<size_t>(len) * cvt.len_mult));
            if (!cvt.buf)
            {
                SDL_FreeWAV(buf);
                return false;
            }
            memcpy(cvt.buf, buf, len);
            SDL_ConvertAudio(&cvt);
            m_trophyPCM.assign(reinterpret_cast<int16_t *>(cvt.buf),
                               reinterpret_cast<int16_t *>(cvt.buf + cvt.len_cvt));
            SDL_free(cvt.buf);
        }

        SDL_FreeWAV(buf);
        LOG_AUDIO("Trophy SFX loaded: %zu samples", m_trophyPCM.size());
        return true;
    }

    /// Restart the chime from the beginning (safe to call mid-playback).
    void PlayTrophy()
    {
        if (m_trophyPCM.empty())
            return;
        m_trophyCursor.store(0, std::memory_order_relaxed);
        m_trophyActive.store(true, std::memory_order_relaxed);
    }

    // Static forwarders so TicoCore can trigger the chime without plumbing a
    // TicoAudio pointer through the core.
    static bool LoadTrophyGlobal(const char *path) { return s_instance ? s_instance->LoadTrophy(path) : false; }
    static void PlayTrophyGlobal() { if (s_instance) s_instance->PlayTrophy(); }

private:
    /// Mix the active trophy chime into an interleaved S16 block in place.
    void MixTrophy(int16_t *dst, size_t numSamples)
    {
        if (!m_trophyActive.load(std::memory_order_relaxed))
            return;
        size_t cursor = m_trophyCursor.load(std::memory_order_relaxed);
        const size_t total = m_trophyPCM.size();
        for (size_t i = 0; i < numSamples; ++i)
        {
            if (cursor >= total)
            {
                m_trophyActive.store(false, std::memory_order_relaxed);
                break;
            }
            int s = static_cast<int>(dst[i]) + static_cast<int>(m_trophyPCM[cursor++]);
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            dst[i] = static_cast<int16_t>(s);
        }
        m_trophyCursor.store(cursor, std::memory_order_relaxed);
    }

    /// SDL_mixer callback - pulls audio when needed
    static void AudioCallback(void *userdata, uint8_t *stream, int len)
    {
        TicoAudio *self = static_cast<TicoAudio *>(userdata);
        if (!self)
        {
            memset(stream, 0, len);
            return;
        }

        // Read from ring buffer
        size_t available = self->m_buffer.Available();
        if (available > 4096)
            available = 4096; // Limit batch size

        if (available > 0)
        {
            int16_t tempBuf[4096];
            size_t read = self->m_buffer.Read(tempBuf, available);

            // Feed to resampler or directly to stream
            if (self->m_resampler)
            {
                SDL_AudioStreamPut(self->m_resampler, tempBuf, read * sizeof(int16_t));
            }
            else
            {
                // No resampling needed - copy directly
                size_t bytesToCopy = std::min(read * sizeof(int16_t), static_cast<size_t>(len));
                memcpy(stream, tempBuf, bytesToCopy);
                if (bytesToCopy < static_cast<size_t>(len))
                {
                    memset(stream + bytesToCopy, 0, len - bytesToCopy);
                }
                return;
            }
        }

        // Get resampled data
        int bytesRead = 0;
        if (self->m_resampler)
        {
            bytesRead = SDL_AudioStreamGet(self->m_resampler, stream, len);
        }

        // Fill underflow with silence
        if (bytesRead < len)
        {
            if (bytesRead < 0)
                bytesRead = 0;
            memset(stream + bytesRead, 0, len - bytesRead);
        }
    }

    TicoRingBuffer<int16_t> m_buffer;
    SDL_AudioStream *m_resampler;
    SDL_AudioDeviceID m_deviceId;
    bool m_initialized;
    bool m_paused;
    bool m_fastForward = false;
    int m_coreSampleRate;

    // Trophy SFX (mixed into the output stream by MixTrophy).
    std::vector<int16_t> m_trophyPCM;       // interleaved S16 @ SAMPLE_RATE/CHANNELS
    std::vector<int16_t> m_mixScratch;      // reused mix buffer (queue path)
    std::atomic<size_t> m_trophyCursor{0};
    std::atomic<bool> m_trophyActive{false};

    // Set in Init() so TicoCore can reach the live instance for trophy SFX.
    inline static TicoAudio *s_instance = nullptr;
};
