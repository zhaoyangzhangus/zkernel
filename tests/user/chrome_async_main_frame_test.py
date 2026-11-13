"""Compile the production dispatch method with the native OS target defined; no inline commit."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
chromium = chromium_source()
source = (chromium/'cc/trees/single_thread_proxy.cc').read_text()
start = source.index('void SingleThreadProxy::ScheduledActionSendBeginMainFrame(')
opening = source.index('{', start)
end, depth = opening+1, 1
while depth:
    depth += (source[end]=='{')-(source[end]=='}')
    end += 1
method = source[start:end]
assert 'PostTask(' in method and 'weak_factory_.GetWeakPtr()' in method
assert 'BeginMainFrame(begin_frame_args);' not in method
pump = (chromium/'base/message_loop/message_pump_epoll.cc').read_text()
assert 'Delegate::NextWorkInfo next_work_info = delegate->DoWork();' in pump
aliases = (chromium/'base/message_loop/message_pump_for_ui.h').read_text()
assert 'using MessagePumpForUI = MessagePumpEpoll;' in aliases

program = r'''
#include <cassert>
#include <functional>
#include <memory>
#include <queue>
#define DCHECK_IS_ON() 0
#define TRACE_EVENT0(...) ((void)0)
#define FROM_HERE 0
namespace viz { struct BeginFrameArgs { int sequence; }; }
struct Runner {
 std::queue<std::function<void()>> tasks;
 void PostTask(int, std::function<void()> task) { tasks.push(std::move(task)); }
 void Drain() { while(!tasks.empty()) {auto task=std::move(tasks.front()); tasks.pop(); task();} }
};
struct Provider { Runner runner; Runner* MainThreadTaskRunner() { return &runner; } };
struct Host { int sent=0; void WillSendBeginMainFrame() {++sent;} };
struct DebugScopedSetImplThread { explicit DebugScopedSetImplThread(Provider*) {} };
struct SingleThreadProxy;
struct Weak { SingleThreadProxy* pointer; std::weak_ptr<int> alive; };
struct Factory {
 SingleThreadProxy* pointer;
 std::shared_ptr<int> alive=std::make_shared<int>(0);
 Weak GetWeakPtr() { return {pointer,alive}; }
 void Invalidate() { alive.reset(); }
};
struct SingleThreadProxy {
 Provider* task_runner_provider_;
 Host* host_impl_;
 Factory weak_factory_{this};
 int property_a=0, property_b=0, commits=0, committed_a=-1, committed_b=-1, sequence=0;
 void ScheduledActionSendBeginMainFrame(const viz::BeginFrameArgs&);
 void BeginMainFrame(const viz::BeginFrameArgs& args) {
  ++commits; committed_a=property_a; committed_b=property_b; sequence=args.sequence;
 }
};
namespace base {
static std::function<void()> BindOnce(void (SingleThreadProxy::*method)(const viz::BeginFrameArgs&),
                                    Weak weak, viz::BeginFrameArgs args) {
 return [=] { if(!weak.alive.expired()) (weak.pointer->*method)(args); };
}
}
''' + method + r'''
int main() {
 Provider provider; Host host;
 SingleThreadProxy proxy{&provider,&host};
 proxy.property_a=1;
 viz::BeginFrameArgs args{42};
 proxy.ScheduledActionSendBeginMainFrame(args);
 assert(host.sent==1 && proxy.commits==0 && provider.runner.tasks.size()==1);
 // 同一个原生回调中的第二次属性修改不能落在过早的内联提交之后。提交的帧应观察到一致状态。
 proxy.property_a=3; proxy.property_b=9; args.sequence=99;
 provider.runner.Drain();
 assert(proxy.commits==1 && proxy.committed_a==3 && proxy.committed_b==9);
 assert(proxy.sequence==42); // Capture args by value, not a dead stack reference.
 proxy.ScheduledActionSendBeginMainFrame({43});
 proxy.weak_factory_.Invalidate();
 provider.runner.Drain();
 assert(proxy.commits==1); // Destroyed/invalidated proxy cannot receive queued work.
}
'''
with tempfile.TemporaryDirectory(prefix='system-async-main-frame-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(program)
    subprocess.run(['c++','-std=c++17','-D__NATIVE_OS__','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.cc'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Async UI frame: production dispatch, coherent properties, args lifetime, weak cancellation and epoll work drain passed')
