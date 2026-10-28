"""Actual shell publication helpers on a simulated fixed-permission mount."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
text=(root/'tools/stage-chrome.sh').read_text()
helpers=text[text.index('declare -A stage_mode_cache=()'):text.index('\nmkdir -p "$esp_dir/EFI/OS/fonts"')]
with tempfile.TemporaryDirectory(prefix='chrome fixed mode ') as directory:
    base=Path(directory)
    (base/'source').write_bytes(b'original resource')
    (base/'target').write_bytes(b'original resource')
    (base/'target').chmod(0o777)
    script=helpers+r'''
set -euo pipefail
cd "$1"
copies=0
chmod() { shift; command chmod 777 "$@"; }
install() { copies=$((copies+1)); shift 2; command install -m 777 "$@"; }
install_data "$PWD/source" "$PWD/target"
[[ "$copies" == 0 ]]
[[ "$(stat -c %a target)" == 777 ]]
# Cache hit still compares content, and changed content is published once.
printf changed > source
install_data "$PWD/source" "$PWD/target"
[[ "$copies" == 1 ]]
cmp source target
install_data "$PWD/source" "$PWD/target"
[[ "$copies" == 1 ]]
[[ ${#stage_mode_cache[@]} == 1 ]]
[[ -z "$(find . -name '.chrome-mode.*' -o -name '*.stage.*')" ]]
'''
    subprocess.run(['bash','-c',script,'test',str(base)],check=True,timeout=10)
print('Chrome resource staging: fixed permissions cache, changed content and probe cleanup passed')
