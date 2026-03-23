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
    static constexpr size_t QUEUE_THRESHOLD = 4096 * 2;    // ~46ms latency target (SDL_QueueAudio)

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

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            // Block if queue full (backpressure)
            while (SDL_GetQueuedAudioSize(m_deviceId) > QUEUE_THRESHOLD && !m_paused)
            {
                SDL_Delay(1);
            }
            int16_t samples[2] = {left, right};
            SDL_QueueAudio(m_deviceId, samples, sizeof(samples));
        }
        else
        {
            // Block if buffer is full (backpressure)
            while (m_buffer.GetAvailableWrite() < 2 && !m_paused)
            {
                SDL_Delay(1);
            }
            int16_t samples[2] = {left, right};
            m_buffer.Write(samples, 2);
        }
    }

    /// Push a batch of audio samples
    size_t PushSamples(const int16_t *data, size_t frames)
    {
        if (!m_initialized || !data || frames == 0)
            return 0;

        size_t samplesNeeded = frames * CHANNELS;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            // Block if queue full (backpressure)
            while (SDL_GetQueuedAudioSize(m_deviceId) > QUEUE_THRESHOLD && !m_paused)
            {
                SDL_Delay(1);
            }
            SDL_QueueAudio(m_deviceId, data, samplesNeeded * sizeof(int16_t));
        }
        else
        {
            // Block if buffer is full (backpressure)
            while (m_buffer.GetAvailableWrite() < samplesNeeded && !m_paused)
            {
                SDL_Delay(1);
            }
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

private:
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
    int m_coreSampleRate;
};
