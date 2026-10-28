"""Real read-only planner: large graph gates, malformed output and errors."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='chrome-build-plan-') as directory:
    base = Path(directory)
    out = base/'output with spaces'
    out.mkdir()
    (out/'build.ninja').write_text('fixture graph\n')
    bin_dir = base/'bin'
    bin_dir.mkdir()
    ninja = bin_dir/'ninja'
    ninja.write_text('#!'+sys.executable+'\nimport os,sys\n'
                     'assert sys.argv[3:]==["-n","-d","explain","chrome"]\n'
                     'print(os.environ["PLAN_OUTPUT"])\n'
                     'sys.exit(int(os.environ.get("PLAN_STATUS","0")))\n')
    ninja.chmod(0o755)
    environment = dict(os.environ, PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
    def run(output, status=0, limit=500):
        return subprocess.run([sys.executable,str(root/'tools/chrome-build-plan.py'),
                               '--out',str(out),'--max-steps',str(limit)],
                              env=dict(environment,PLAN_OUTPUT=output,PLAN_STATUS=str(status)),
                              capture_output=True,text=True,timeout=5)
    for steps in (1,10,500,501,1145):
        result = run(f'ninja explain: fixture dependency changed\n[1/{steps}] CXX fixture.o')
        assert result.returncode==(0 if steps<=500 else 2)
        assert f'steps={steps} limit=500' in result.stdout
    assert run('[1/1145] CXX fixture.o',limit=1200).returncode==0
    assert run('ninja: no work to do.').returncode==0
    assert run('ninja: Entering directory only').returncode==1
    assert run('ninja: error: fixture',status=7).returncode==7
    assert run('[1/1] CXX fixture.o',limit=0).returncode==2
    assert (out/'build.ninja').read_text()=='fixture graph\n'
    (out/'build.ninja').unlink()
    assert run('ninja: no work to do.').returncode==2
print('Chrome plan: dry-run only, thresholds/override, no-op, failures, unknown output and missing graph passed')
