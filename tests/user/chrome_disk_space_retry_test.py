"""直接编译 Chromium 的空间评估实现，验证失败退避和过期重试。"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(os.environ['CHROMIUM_SRC'])
source = (root / 'components/optimization_guide/core/model_execution/manifest_broker/manifest_asset_manager.cc').read_text()
a = source.index('void ManifestAssetManager::DiskSpaceStatus::Update(')
b = source.index('bool ManifestAssetManager::DiskSpaceStatus::CanSupportOnDemandInstall()', a)
methods = source[a:b].replace('ManifestAssetManager::DiskSpaceStatus::', 'Status::')
code = r'''
#include <cassert>
#include <cstdint>
#include <optional>
namespace base {
using ByteSize = uint64_t;
static int64_t now = 100;
struct Time {
 int64_t value = 0;
 static Time Now() { return {now}; }
 bool is_null() const { return value == 0; }
 friend int64_t operator-(Time a, Time b) { return a.value-b.value; }
};
}
constexpr int64_t kDiskSpaceFreshnessThreshold = 10;
struct Status {
 std::optional<base::ByteSize> free_space_;
 base::Time last_evaluated_;
 void Update(std::optional<base::ByteSize>);
 bool IsFresh() const;
};
''' + methods + r'''
int main() {
 Status state;
 assert(!state.IsFresh());
 state.Update(std::nullopt);
 assert(state.IsFresh() && !state.free_space_.has_value());
 for (unsigned i=0;i<100000;++i) assert(state.IsFresh());
 base::now += 9;
 assert(state.IsFresh());
 ++base::now;
 assert(!state.IsFresh());
 state.Update(4096);
 assert(state.IsFresh() && state.free_space_ == 4096);
 base::now += 10;
 assert(!state.IsFresh());
 state.Update(std::nullopt);
 assert(state.IsFresh() && !state.free_space_);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-space-retry-') as directory:
    binary = Path(directory) / 'test'
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c++', '-', '-o', str(binary)], input=code, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: real Chromium disk-space methods: unknown result is bounded, 10-second expiry and successful refresh')
