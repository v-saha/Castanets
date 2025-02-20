// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/audio_output_device_thread_callback.h"

#include <utility>

#include "base/metrics/histogram_macros.h"
#include "base/trace_event/trace_event.h"

#if defined(CASTANETS)
#include "base/distributed_chromium_util.h"
#include "mojo/public/cpp/system/sync.h"
#endif

namespace media {

AudioOutputDeviceThreadCallback::Metrics::Metrics()
    : first_play_start_time_(base::nullopt) {}

AudioOutputDeviceThreadCallback::Metrics::~Metrics() = default;

void AudioOutputDeviceThreadCallback::Metrics::OnCreated() {
  start_time_ = base::TimeTicks::Now();
}

void AudioOutputDeviceThreadCallback::Metrics::OnProcess() {
  if (first_play_start_time_) {
    UMA_HISTOGRAM_TIMES("Media.Audio.Render.OutputDeviceStartTime",
                        base::TimeTicks::Now() - *first_play_start_time_);
  }
}

void AudioOutputDeviceThreadCallback::Metrics::OnInitializePlayStartTime() {
  if (!first_play_start_time_.has_value())
    first_play_start_time_ = base::TimeTicks::Now();
}

void AudioOutputDeviceThreadCallback::Metrics::OnDestroyed() {
  DCHECK(!start_time_.is_null());
  UMA_HISTOGRAM_LONG_TIMES("Media.Audio.Render.OutputStreamDuration",
                           base::TimeTicks::Now() - start_time_);
}

AudioOutputDeviceThreadCallback::AudioOutputDeviceThreadCallback(
    const media::AudioParameters& audio_parameters,
    base::UnsafeSharedMemoryRegion shared_memory_region,
    media::AudioRendererSink::RenderCallback* render_callback,
    std::unique_ptr<Metrics> metrics)
    : media::AudioDeviceThread::Callback(
          audio_parameters,
          ComputeAudioOutputBufferSize(audio_parameters),
          /*segment count*/ 1),
      shared_memory_region_(std::move(shared_memory_region)),
      render_callback_(render_callback),
      callback_num_(0),
      metrics_(std::move(metrics)) {
  // CHECK that the shared memory is large enough. The memory allocated must be
  // at least as large as expected.
  CHECK(memory_length_ <= shared_memory_region_.GetSize());
  if (metrics_)
    metrics_->OnCreated();
}

AudioOutputDeviceThreadCallback::~AudioOutputDeviceThreadCallback() {
  if (metrics_)
    metrics_->OnDestroyed();
}

void AudioOutputDeviceThreadCallback::MapSharedMemory() {
  CHECK_EQ(total_segments_, 1u);
  shared_memory_mapping_ = shared_memory_region_.MapAt(0, memory_length_);
  CHECK(shared_memory_mapping_.IsValid());

  media::AudioOutputBuffer* buffer =
      reinterpret_cast<media::AudioOutputBuffer*>(
          shared_memory_mapping_.memory());
  output_bus_ = media::AudioBus::WrapMemory(audio_parameters_, buffer->audio);
  output_bus_->set_is_bitstream_format(audio_parameters_.IsBitstreamFormat());
}

// Called whenever we receive notifications about pending data.
void AudioOutputDeviceThreadCallback::Process(uint32_t control_signal) {
  callback_num_++;

#if defined(CASTANETS)
  if (base::Castanets::IsEnabled())
    mojo::WaitSyncSharedMemory(shared_memory_region_.GetGUID());
#endif
  // Read and reset the number of frames skipped.
  media::AudioOutputBuffer* buffer =
      reinterpret_cast<media::AudioOutputBuffer*>(
          shared_memory_mapping_.memory());
  uint32_t frames_skipped = buffer->params.frames_skipped;
  buffer->params.frames_skipped = 0;

  TRACE_EVENT_BEGIN2("audio", "AudioOutputDevice::FireRenderCallback",
                     "callback_num", callback_num_, "frames skipped",
                     frames_skipped);

  base::TimeDelta delay =
      base::TimeDelta::FromMicroseconds(buffer->params.delay_us);

  base::TimeTicks delay_timestamp =
      base::TimeTicks() +
      base::TimeDelta::FromMicroseconds(buffer->params.delay_timestamp_us);

  DVLOG(4) << __func__ << " delay:" << delay << " delay_timestamp:" << delay
           << " frames_skipped:" << frames_skipped;

  // When playback starts, we get an immediate callback to Process to make sure
  // that we have some data, we'll get another one after the device is awake and
  // ingesting data, which is what we want to track with this trace.
  if (callback_num_ == 2) {
    if (metrics_)
      metrics_->OnProcess();
    TRACE_EVENT_ASYNC_END0("audio", "StartingPlayback", this);
  }

  // Update the audio-delay measurement, inform about the number of skipped
  // frames, and ask client to render audio.  Since |output_bus_| is wrapping
  // the shared memory the Render() call is writing directly into the shared
  // memory.
  render_callback_->Render(delay, delay_timestamp, frames_skipped,
                           output_bus_.get());

  if (audio_parameters_.IsBitstreamFormat()) {
    buffer->params.bitstream_data_size = output_bus_->GetBitstreamDataSize();
    buffer->params.bitstream_frames = output_bus_->GetBitstreamFrames();
  }

#if defined(CASTANETS)
  if (base::Castanets::IsEnabled()) {
    mojo::SyncSharedMemory(shared_memory_region_.GetGUID(), 0,
                           shared_memory_region_.GetSize());
  }
#endif

  TRACE_EVENT_END2("audio", "AudioOutputDevice::FireRenderCallback",
                   "timestamp (ms)",
                   (delay_timestamp - base::TimeTicks()).InMillisecondsF(),
                   "delay (ms)", delay.InMillisecondsF());
}

bool AudioOutputDeviceThreadCallback::CurrentThreadIsAudioDeviceThread() {
  return thread_checker_.CalledOnValidThread();
}

void AudioOutputDeviceThreadCallback::InitializePlayStartTime() {
  if (metrics_)
    metrics_->OnInitializePlayStartTime();
}

}  // namespace media
