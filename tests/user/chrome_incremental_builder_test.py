"""Run the actual bounded builder payload against fake GN/Ninja executables."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'tools/build-chrome.sh').read_text()
payload=re.search(r"    '(set -euo pipefail\n.*?exec ninja[^']*)' \\",source,re.S).group(1)
service_environment='env SCCACHE_CACHE_SIZE=30G OS_ROOT="$project_root" OS_CHROME_MAX_BUILD_STEPS=500 /bin/bash -c'
assert 'OS_CHROME_MAX_BUILD_STEPS="$max_build_steps" /bin/bash -c' in source
assert 'python3 "$OS_ROOT/tools/chrome-build-plan.py"' in payload
assert 'gen out/liteos-debian --threads=1' in payload
assert 'exec ninja -C out/liteos-debian -j12 chrome' in payload
assert '--unit=system-chromium-build' in source and '-p KillMode=control-group' in source
assert '-p MemoryMax=24G' in source and '-p CPUQuota=1200%' in source
assert 'build_log="${OS_CHROME_BUILD_LOG:-$project_root/build/chrome-unified-build.log}"' in source
assert '>>"$build_log" 2>&1' in source
assert 'env SCCACHE_CACHE_SIZE=30G OS_ROOT="$project_root"' in source
assert '[[ -d "$(dirname -- "$build_log")" ]]' in source
rule='rule gn\n  command = ../../buildtools/linux64/gn --root=../.. -q --threads=1 --regeneration gen .\n  pool = console\n'
with tempfile.TemporaryDirectory(prefix='chrome-incremental-') as directory:
    base=Path(directory)
    src=base/'source with spaces'; out=src/'out/liteos-debian'; out.mkdir(parents=True)
    gn=src/'buildtools/linux64/gn'; gn.parent.mkdir(parents=True)
    commands=base/'commands'; commands.mkdir()
    trace=base/'trace'; graph=out/'build.ninja'
    for path, name in ((gn,'GN'),(commands/'ninja','NINJA')):
        path.write_text('#!/bin/sh\n'
                        '[ "$OS_ROOT" = "$EXPECTED_ROOT" ] || exit 97\n'
                        '[ "$SCCACHE_CACHE_SIZE" = "30G" ] || exit 98\n'
                        f'printf "{name}" >> "$TRACE"\n'
                        'printf " <%s>" "$@" >> "$TRACE"\n'
                        'printf "\\n" >> "$TRACE"\n'
                        f'exit "${{{name}_EXIT:-0}}"\n')
        path.chmod(0o755)
    selected_root=str(base/'target root with spaces')
    planner=Path(selected_root)/'tools/chrome-build-plan.py'
    planner.parent.mkdir(parents=True)
    planner.write_text('import os,sys\n'
                      'with open(os.environ["TRACE"],"a") as f:f.write("PLAN\\n")\n'
                      'sys.exit(int(os.environ.get("PLAN_EXIT","0")))\n')
    env=dict(os.environ,PATH=str(commands)+os.pathsep+os.environ['PATH'],TRACE=str(trace),
             OS_ROOT='/unrelated/ambient-root', EXPECTED_ROOT=selected_root,
             SCCACHE_CACHE_SIZE='1G')
    def run(text, gn_exit=0, ninja_exit=0, plan_exit=0):
        if graph.exists(): graph.unlink()
        if text is not None: graph.write_text(text)
        trace.write_text('')
        result=subprocess.run(['bash','-c', 'project_root="$1"\n'+service_environment+' "$2"',
                               'builder-test', selected_root, payload],cwd=src,
                              env=dict(env,GN_EXIT=str(gn_exit),NINJA_EXIT=str(ninja_exit),PLAN_EXIT=str(plan_exit)),
                              capture_output=True,text=True)
        if text is None: assert not graph.exists()
        else: assert graph.read_text()==text
        return result.returncode,trace.read_text().splitlines()
    gn_line='GN <gen> <out/liteos-debian> <--threads=1>'
    ninja_line='NINJA <-C> <out/liteos-debian> <-j12> <chrome>'
    assert run(rule)==(0,['PLAN',ninja_line])
    for graph_text in (None,'',rule.replace('--threads=1','--threads=12'),
                       rule.replace('--threads=1','--threads=10'),
                       rule.replace(' --threads=1',''),
                       rule.replace('--regeneration','--other'),
                       'rule unrelated\n'+rule.split('\n',1)[1],
                       rule.replace('--threads=1','--threads=12')+'rule other\n'+rule.split('\n',1)[1]):
        assert run(graph_text)==(0,[gn_line,'PLAN',ninja_line]),graph_text
    assert run(None,gn_exit=27)==(27,[gn_line])
    assert run(rule,gn_exit=27)==(0,['PLAN',ninja_line])
    assert run(rule,ninja_exit=31)==(31,['PLAN',ninja_line])
    assert run(None,ninja_exit=31)==(31,[gn_line,'PLAN',ninja_line])
    assert run(rule,plan_exit=2)==(2,['PLAN'])
    assert run(None,plan_exit=1)==(1,[gn_line,'PLAN'])
print('Chrome builder: existing single-thread graph avoids GN; bootstrap, unsafe rules and failure propagation passed')
