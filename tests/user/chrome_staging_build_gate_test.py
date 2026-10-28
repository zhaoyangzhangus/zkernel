"""Exercise the production staging gate without copying any browser payload."""
from pathlib import Path
import fcntl
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'tools/stage-chrome.sh').read_text()
start = source.index('# 增量构建期间，旧的已链接二进制仍然存在。')
end = source.index('# 原生移植版的 H.264',start)
gate = source[start:end]
with tempfile.TemporaryDirectory(prefix='chrome-stage-gate-') as directory:
    path = Path(directory)
    commands = path/'bin'
    commands.mkdir()
    build = path/'build'
    build.mkdir()
    out = path/'out'
    out.mkdir()
    launcher = build/'elf/chrome-start.elf'
    launcher.parent.mkdir()
    launcher.touch()
    marker = path/'copied'
    calls = path/'ninja-calls'
    scripts = {
        'make':'exit "${TEST_LAUNCHER_STATUS:-0}"',
        'pgrep':'exit "${TEST_NINJA_RUNNING:-1}"',
        'systemctl':'''if [ -n "${TEST_ACTIVE_UNIT:-}" ]; then
  for arg do [ "$arg" = "$TEST_ACTIVE_UNIT" ] && exit 0; done
  exit 1
fi
exit "${TEST_UNIT_RUNNING:-1}"''',
        'ninja':'''printf '%s\\n' "$*" >> "$TEST_NINJA_CALLS"
case "$TEST_GRAPH" in
  clean) printf '%s\\n' 'ninja: Entering directory' 'ninja: no work to do.' ;;
  dirty) printf '%s\\n' '[1/2] CXX modified.cc' '[2/2] LINK chrome' ;;
  deceptive) printf '%s\\n' 'echo "ninja: no work to do."' ;;
  failed) exit 1 ;;
  *) exit 99 ;;
esac''',
    }
    for name,body in scripts.items():
        script = commands/name
        script.write_text('#!/bin/sh\n'+body+'\n')
        script.chmod(0o755)
    program = 'set -euo pipefail\n'+gate+'\nprintf reached > "$TEST_MARKER"\n'
    environment = {**os.environ,'PATH':str(commands)+':'+os.environ['PATH'],
                   'build_dir':str(build),'chromium_out':str(out),
                   'project_root':str(root),'launcher_source':str(launcher),
                   'TEST_MARKER':str(marker),'TEST_NINJA_CALLS':str(calls)}

    def check(case, *, running=1, unit=1, graph='clean', accepted=False,
              reads_graph=False, active_unit='', launcher_status=0):
        marker.unlink(missing_ok=True)
        calls.unlink(missing_ok=True)
        result = subprocess.run(['/bin/bash','-c',program],env={**environment,
            'TEST_NINJA_RUNNING':str(running),'TEST_UNIT_RUNNING':str(unit),
            'TEST_GRAPH':graph,'TEST_ACTIVE_UNIT':active_unit,
            'TEST_LAUNCHER_STATUS':str(launcher_status)},text=True,capture_output=True,timeout=5)
        assert (result.returncode==0)==accepted, (case,result.stderr)
        assert marker.exists()==accepted, case
        assert calls.exists()==reads_graph, case
        if reads_graph:
            assert calls.read_text().strip()=='-n -C '+str(out)+' chrome',case

    check('live Ninja',running=0)
    check('live service after launching shell exited',unit=0)
    for name in ('system-chromium-build.service', 'system-chromium-build.scope',
                 'liteos-chromium-build.service', 'liteos-chromium-build.scope'):
        check('live unit ' + name, active_unit=name)
    with (build/'.chromium-build.lock').open('w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        check('another builder/stager holds lock')
    check('unfinished link',graph='dirty',reads_graph=True)
    check('no-work text inside a pending command',graph='deceptive',reads_graph=True)
    check('failed graph load',graph='failed',reads_graph=True)
    check('outdated launcher',launcher_status=1,reads_graph=True)
    check('launcher graph error',launcher_status=2,reads_graph=True)
    launcher.unlink()
    check('missing launcher despite clean graph',reads_graph=True)
    launcher.touch()
    check('completed build',accepted=True,reads_graph=True)
print('Chrome staging gate: live job/unit/lock, dirty/failed browser or launcher graph and missing launcher rejected; completed build accepted')
