"""Compile the native Chromium pressure policy and evaluator unchanged."""
from pathlib import Path
import resource
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

src = (chromium_source() / 'components/memory_pressure/system_memory_pressure_evaluator.cc').read_text()
begin = src.index('base::MemoryPressureLevel NativeMemoryPressureLevel(')
end = src.index('\n}  // namespace', begin)
production = src[begin:end]
assert 'std::make_unique<NativeSystemMemoryPressureEvaluator>' in src
program = r'''
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <cstdint>
#include <cassert>
#include <functional>
#define sysinfo fixture_sysinfo
#include <sys/sysinfo.h>
static struct sysinfo sample;
static int sample_error;
static unsigned reads;
int sysinfo(struct sysinfo *out) noexcept { ++reads; *out=sample; return sample_error; }
#define FROM_HERE 0
namespace base {
enum MemoryPressureLevel { MEMORY_PRESSURE_LEVEL_NONE,
 MEMORY_PRESSURE_LEVEL_MODERATE, MEMORY_PRESSURE_LEVEL_CRITICAL };
struct TimeDelta { int64_t value; auto operator<=>(const TimeDelta&)const=default; };
constexpr TimeDelta Seconds(int64_t n){return {n};}
static int64_t clock_now=1;
struct TimeTicks {
 int64_t value=0; static TimeTicks Now(){return {clock_now};}
 bool is_null()const{return !value;}
 TimeDelta operator-(TimeTicks other)const{return {value-other.value};}
};
class RepeatingTimer {
 public:
 static inline std::function<void()> callback;
 template<class T>void Start(int,TimeDelta period,T *owner,void(T::*method)()) {
  assert(period.value==1 && !callback);callback=[=]{(owner->*method)();};
 }
 ~RepeatingTimer(){callback={};}
};
}
static unsigned votes,notifications;
struct MemoryPressureVoter {};
class SystemMemoryPressureEvaluator {
 public:
 static constexpr auto kRenotifyVotePeriod=base::Seconds(5);
 virtual ~SystemMemoryPressureEvaluator()=default;
 base::MemoryPressureLevel current_vote()const{return level_;}
 protected:
 explicit SystemMemoryPressureEvaluator(std::unique_ptr<MemoryPressureVoter> v) {
  assert(v);
 }
 void SetCurrentVote(base::MemoryPressureLevel l){level_=l;}
 void SendCurrentVote(bool notify)const{++votes;notifications+=notify;}
 private:
 base::MemoryPressureLevel level_=base::MEMORY_PRESSURE_LEVEL_NONE;
};
''' + production + r'''
static void available(uint64_t n){sample.freeram=n;}
static void tick(int64_t now){base::clock_now=now;base::RepeatingTimer::callback();}
int main(){
 using namespace base;
 const uint64_t gib=uint64_t{1}<<30;
 sample.totalram=4*gib;sample.freeram=3*gib;sample.mem_unit=1;
 {
  NativeSystemMemoryPressureEvaluator evaluator(std::make_unique<MemoryPressureVoter>());
  assert(reads==1 && !votes && evaluator.current_vote()==MEMORY_PRESSURE_LEVEL_NONE);
  available(gib/2);tick(2);
  assert(votes==1 && notifications==1 && evaluator.current_vote()==MEMORY_PRESSURE_LEVEL_MODERATE);
  tick(6);assert(votes==1 && notifications==1);
  tick(7);assert(votes==2 && notifications==2);
  available(gib/4);tick(8);
  assert(votes==3 && notifications==3 && evaluator.current_vote()==MEMORY_PRESSURE_LEVEL_CRITICAL);
  sample_error=-1;tick(9);assert(votes==3);
  sample_error=0;sample.mem_unit=0;tick(10);assert(votes==3);
  sample.mem_unit=1;sample.freeram=sample.totalram+1;tick(11);assert(votes==3);
  sample.totalram=UINT64_MAX;sample.freeram=0;sample.mem_unit=2;tick(12);assert(votes==3);
  sample.totalram=4*gib;sample.mem_unit=1;
  available(sample.totalram/10+1);tick(13);
  assert(evaluator.current_vote()==MEMORY_PRESSURE_LEVEL_CRITICAL && notifications==4);
  available(sample.totalram/10+sample.totalram/40);tick(14);
  assert(evaluator.current_vote()==MEMORY_PRESSURE_LEVEL_MODERATE && notifications==5);
  available(sample.totalram/5+1);tick(15);assert(votes==5);
  available(sample.totalram/5+sample.totalram/40);tick(16);
  assert(evaluator.current_vote()==MEMORY_PRESSURE_LEVEL_NONE && votes==6 && notifications==5);
  tick(30);assert(votes==6 && notifications==5);
  // sysinfo is allowed to report counts in pages, not bytes.
  sample.totalram=1048576;sample.freeram=1000;sample.mem_unit=4096;tick(31);
  assert(evaluator.current_vote()==MEMORY_PRESSURE_LEVEL_CRITICAL && notifications==6);
 }
 assert(!RepeatingTimer::callback);
 for(uint64_t total: {gib/16,gib,4*gib,32*gib,UINT64_MAX}) {
  const uint64_t critical=std::min(total/10,uint64_t{512}<<20);
  const uint64_t moderate=std::min(total/5,uint64_t{1024}<<20);
  const uint64_t margin=std::min(total/40,uint64_t{128}<<20);
  for(auto previous:{MEMORY_PRESSURE_LEVEL_NONE,MEMORY_PRESSURE_LEVEL_MODERATE,MEMORY_PRESSURE_LEVEL_CRITICAL}) {
   for(uint64_t free:{uint64_t{0},critical,critical+1,critical+margin,
                     moderate,moderate+1,moderate+margin,total}) {
    auto expected=MEMORY_PRESSURE_LEVEL_NONE;
    if(free<=moderate || (previous!=MEMORY_PRESSURE_LEVEL_NONE && free<moderate+margin))expected=MEMORY_PRESSURE_LEVEL_MODERATE;
    if(free<=critical || (previous==MEMORY_PRESSURE_LEVEL_CRITICAL && free<critical+margin))expected=MEMORY_PRESSURE_LEVEL_CRITICAL;
    assert(NativeMemoryPressureLevel(total,free,previous)==expected);
   }
  }
 }
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='chrome-memory-pressure-') as directory:
    p = Path(directory)
    code = p / 'test.cc'
    for negative in (False, True):
        body = program
        if negative:
            body = body.replace('if (level == previous && !notify)', 'if (!notify)')
            assert body != program
        code.write_text(body)
        subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(code), '-o', str(p/'test')], check=True)
        result = subprocess.run([str(p/'test')], capture_output=True)
        assert (result.returncode != 0) == negative, result.stderr.decode()
print('Chrome native pressure: production policy/evaluator, thresholds/hysteresis, '
      'invalid samples, units/overflow, repeated notifications, recovery and timer '
      'lifetime PASS; stuck-pressure negative control rejected')
