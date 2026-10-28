"""Execute the real nativeguest protocol gate without rebuilding Chromium tests."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = chromium_source()
source = (root / 'chrome/browser/external_protocol/external_protocol_handler.cc').read_text()
start = source.index('ExternalProtocolHandler::BlockState ExternalProtocolHandler::GetBlockState(')
end = source.index('  LogRequestForScheme(scheme);', start)
gate = source[start:end]
fixture = r'''
#include <cassert>
#include <string>
#define BUILDFLAG(x) x
#define IS_LINUX TEST_LINUX
#define DCHECK_CURRENTLY_ON(...) ((void)0)
namespace url { class Origin {}; }
class Profile {};
namespace base {
class CommandLine {
 public:
  std::string platform;
  static CommandLine* ForCurrentProcess() { static CommandLine value; return &value; }
  std::string GetSwitchValueASCII(const char* key) {
    assert(std::string(key) == "ozone-platform"); return platform;
  }
};
}
class ExternalProtocolHandler {
 public:
  enum BlockState { BLOCK, UNKNOWN };
  static BlockState GetBlockState(const std::string&, const url::Origin*, Profile*);
};
''' + gate + r'''
  (void)scheme; (void)initiating_origin; (void)profile;
  return UNKNOWN; // Sentinel for the unchanged desktop policy path.
}
int main() {
  for (const char* platform : {"nativeguest", "x11", "wayland", "", "nativeguest-other"}) {
    base::CommandLine::ForCurrentProcess()->platform = platform;
    for (const char* scheme : {"snssdk1128", "bytedance", "tel", "mailto"}) {
      auto expected = TEST_LINUX && std::string(platform) == "nativeguest"
          ? ExternalProtocolHandler::BLOCK : ExternalProtocolHandler::UNKNOWN;
      assert(ExternalProtocolHandler::GetBlockState(scheme, nullptr, nullptr) == expected);
    }
  }
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-external-protocol-') as directory:
    path = Path(directory)
    for linux in (0, 1):
        binary = path / 'test'
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-DTEST_LINUX=' + str(linux), '-x', 'c++', '-', '-o', str(binary)],
                       input=fixture, text=True, check=True)
        subprocess.run([str(binary)], check=True)
print('External protocols: nativeguest blocks unavailable apps before prompting; other backends/platforms retain policy flow')
