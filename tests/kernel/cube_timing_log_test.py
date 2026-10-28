"""Actual timing reporter: bounded desktop-visible record and exact units."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/nvidia/opengl_cube_demo.c').read_text()
body = source[source.index('static void cube_report_timing('):
              source.index('static int cube_process_events(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tools/nvidia/cube_timing.h"
static char output[256], saved[256];
static unsigned reports;
static void cube_emit(const char *text) { assert(strlen(text) < sizeof(output)); strcpy(output, text); }
static void cube_record_log(const char *path, const char *text) {
    assert(!strcmp(path, "/log/cube-timing.log"));
    assert(strlen(text) < sizeof(saved)); strcpy(saved, text); ++reports;
}
''' + body + r'''
int main(void) {
    cube_timing_totals_t totals = {0};
    cube_report_timing(&totals); assert(!reports);
    totals = (cube_timing_totals_t){.frames=2, .readback_ns=1234000,
        .publish_ns=10000000, .readback_max_ns=900000, .publish_max_ns=6000000};
    cube_report_timing(&totals);
    assert(!strcmp(output, saved) && reports == 1);
    assert(!strcmp(saved, "OS_CUBE_TIMING frames=2 readback_avg_us=617 readback_max_us=900 submit_avg_us=5000 submit_max_us=6000\n"));
    totals.frames=1; totals.readback_ns=totals.publish_ns=UINT64_MAX;
    totals.readback_max_ns=totals.publish_max_ns=UINT64_MAX;
    cube_report_timing(&totals); assert(reports == 2 && strlen(saved) < sizeof(saved));
    puts("Cube persistent timing: actual reporter, zero frames, units and uint64 bounds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='cube-timing-log-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
