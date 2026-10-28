"""提取 Chromium 的实际流调度方法，验证独占声卡上的多音轨生命周期。"""
import os
from pathlib import Path
import subprocess
import tempfile

src = Path(os.environ['CHROMIUM_SRC']) / 'services/audio/output_device_mixer_impl.cc'
text = src.read_text()
names = ['OpenStream', 'StartStream', 'StopStream', 'StopListening',
         'EnsureMixingGraphOutputStreamOpen']
methods = []
for name in names:
    token = 'OutputDeviceMixerImpl::' + name + '('
    start = text.rfind('\n', 0, text.index(token)) + 1
    brace = text.index('{', text.index(token))
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    methods.append(text[start:end])
policy = text[text.index('#if defined(__NATIVE_OS__)'):text.index('// Do not change:')]
code = r'''
#include <cassert>
#include <memory>
#include <set>
#define DCHECK_IS_ON() 0
#define DCHECK(x) assert(x)
#define CHECK(x) assert(x)
#define DCHECK_CALLED_ON_VALID_SEQUENCE(...) ((void)0)
#define NON_REENTRANT_SCOPE(...) ((void)0)
#define TRACE_EVENT1(...) ((void)0)
#define TRACE_EVENT2(...) ((void)0)
#define FROM_HERE 0
struct NullLog { template<class T> NullLog& operator<<(T const&) { return *this; } };
#define DVLOG(...) NullLog()
namespace base { struct AutoLock { explicit AutoLock(int&) {} }; }
namespace media { struct AudioOutputStream { struct AudioSourceCallback {}; }; }
struct Listener {};
struct Sink {};
struct MixTrack {
  int independent = 0, mixed = 0;
  media::AudioOutputStream::AudioSourceCallback* source = nullptr;
  bool OpenIndependentRenderingStream() { ++independent; return true; }
  void StartIndependentRenderingStream() { ++independent; }
  void StopIndependentRenderingStream() {}
  void StartProvidingAudioToMixingGraph() { assert(!mixed && source); ++mixed; }
  void StopProvidingAudioToMixingGraph() { assert(mixed); --mixed; }
  void SetSource(media::AudioOutputStream::AudioSourceCallback* s) {
    assert(!source || !s); source = s;
  }
};
struct Timer {
  bool started = false;
  bool IsRunning() { return started; }
  template<class... T> void Start(T...) { started = true; }
};
struct OutputDeviceMixerImpl {
  enum class MixingError { kNone };
  std::set<MixTrack*> active_tracks_;
  std::set<Listener*> listeners_;
  int listener_lock_ = 0, mixing_graph_output_params_ = 0;
  std::unique_ptr<Sink> mixing_graph_output_stream_;
  Timer switch_to_unmixed_playback_delay_timer_;
  static constexpr int kSwitchToUnmixedPlaybackDelay = 1;
  bool mixing = false, fail_open = false;
  int opens = 0, starts = 0, stops = 0;
  int device_id() { return 0; }
  bool HasListeners() { return !listeners_.empty(); }
  bool MixingInProgress() { return mixing; }
  Sink* CreateAndOpenDeviceStream(int) {
    ++opens; return fail_open ? nullptr : new Sink;
  }
  void StartMixingGraphPlayback() {
    if (!mixing_graph_output_stream_) return;
    assert(!mixing); mixing = true; ++starts;
    for (auto* t : active_tracks_) t->StartProvidingAudioToMixingGraph();
  }
  void StopMixingGraphPlayback(MixingError) {
    assert(mixing); ++stops;
    for (auto* t : active_tracks_) t->StopProvidingAudioToMixingGraph();
    mixing = false; mixing_graph_output_stream_.reset();
  }
  void SwitchToUnmixedPlaybackTimerHelper() { assert(false); }
  bool OpenStream(MixTrack*);
  void StartStream(MixTrack*, media::AudioOutputStream::AudioSourceCallback*);
  void StopStream(MixTrack*);
  void StopListening(Listener*);
  void EnsureMixingGraphOutputStreamOpen();
};
''' + policy + '\n'.join(methods) + r'''
int main() {
  OutputDeviceMixerImpl m;
  MixTrack a, b;
  media::AudioOutputStream::AudioSourceCallback source;
  // 预加载只创建逻辑音轨，不能抢占硬件。
  assert(m.OpenStream(&a) && m.OpenStream(&b));
  assert(!m.opens && !a.independent && !b.independent);
  m.StartStream(&a, &source);
  m.StartStream(&b, &source);
  assert(m.opens == 1 && m.starts == 1 && a.mixed && b.mixed);
  m.StopStream(&a);
  assert(!a.source && !a.mixed && b.mixed && m.mixing);
  m.StopStream(&a);
  m.StopStream(&b);
  assert(!m.mixing && !m.mixing_graph_output_stream_ && m.stops == 1);
  // 停止后重新播放，以及最后一个监听者离开，都不能切回独立声卡流。
  m.StartStream(&b, &source);
  Listener listener;
  m.listeners_.insert(&listener);
  m.StopListening(&listener);
  assert(m.mixing && !m.switch_to_unmixed_playback_delay_timer_.started);
  m.listeners_.insert(&listener);
  m.StopStream(&b);
  assert(m.mixing);
  m.StopListening(&listener);
  assert(!m.mixing && !m.mixing_graph_output_stream_);
  assert(!a.independent && !b.independent);
  // 硬件打开失败后，下一次启动仍可恢复。
  m.fail_open = true;
  m.StartStream(&a, &source);
  assert(!m.mixing && !m.mixing_graph_output_stream_);
  m.StopStream(&a);
  m.fail_open = false;
  m.StartStream(&a, &source);
  assert(m.mixing && a.mixed);
  m.StopStream(&a);
  assert(m.active_tracks_.empty() && !m.mixing);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path/'test.cc').write_text(code)
    subprocess.run(['c++', '-std=c++20', '-D__NATIVE_OS__', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-g',
                    str(path/'test.cc'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('Chromium 实际混音调度：双音轨预加载、并发播放、独立停止、重启、监听者退出、打开失败恢复通过')
