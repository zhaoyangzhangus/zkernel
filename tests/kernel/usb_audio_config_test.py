"""执行真实端点配置函数，验证拒绝参数时不分配或覆盖 DMA 所有权。"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('AUDIO_CONFIG_SOURCE',
                             root / 'kernel/drivers/usb/xhci/audio_runtime.c')).read_text()
body = source[source.index('#define XHCI_RING_TRB_COUNT'):
              source.index('bool xhci_queue_audio_transfer_device')]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "kernel/drivers/usb/xhci/internal.h"
static unsigned allocations, releases, syncs, commands, fail_allocation;
uint64_t xhci_dma_address(const dma_mapping_t *mapping) {
    return mapping->mapped_length;
}
bool xhci_alloc_page(xhci_state_t *state, xhci_dma_page_t *page,
                     enum dma_direction direction) {
    assert(state && !page->page && !page->cpu && !page->mapping.device);
    if (++allocations == fail_allocation) return false;
    page->cpu = calloc(1, PAGE_SIZE);
    assert(page->cpu);
    page->page = (page_t *)page->cpu;
    page->mapping.device = (void *)1;
    page->mapping.mapped_length = page == &state->audio_ring ? 0x1000 : 0x2000;
    page->mapping.direction = direction;
    return true;
}
bool xhci_free_page(xhci_dma_page_t *page) {
    if (page->cpu) { free(page->cpu); ++releases; }
    memset(page, 0, sizeof(*page));
    return true;
}
void dma_sync_for_device(dma_mapping_t *mapping) {
    assert(mapping->mapped_length); ++syncs;
}
bool xhci_submit_command(xhci_state_t *state, uint32_t type, uint8_t slot,
                         uint64_t address, uint8_t *result) {
    assert(state && type == 12 && slot == 4 && address == 0x3000 && !result);
    assert(syncs == 3); ++commands; return true;
}
''' + body + r'''
static _Alignas(64) uint8_t input[4096], output[4096];
static xhci_state_t fresh(void) {
    allocations = releases = syncs = commands = fail_allocation = 0;
    memset(input, 0x35, sizeof(input)); memset(output, 0x46, sizeof(output));
    xhci_state_t state = {0};
    state.device_slot = 4; state.device_speed = 3; state.context_size = 32;
    state.audio_endpoint = 5; state.audio_interval = 1; state.audio_max_packet = 248;
    state.input_context.cpu = input; state.output_context.cpu = output;
    state.input_context.mapping.mapped_length = 0x3000;
    return state;
}
static void rejected(xhci_state_t *state) {
    xhci_state_t saved = *state;
    uint8_t saved_input[4096], saved_output[4096];
    memcpy(saved_input, input, sizeof(input)); memcpy(saved_output, output, sizeof(output));
    assert(!xhci_configure_audio_endpoint(state));
    assert(!allocations && !releases && !syncs && !commands);
    assert(!memcmp(&saved, state, sizeof(saved)));
    assert(!memcmp(saved_input, input, sizeof(input)));
    assert(!memcmp(saved_output, output, sizeof(output)));
}
int main(void) {
    assert(!xhci_configure_audio_endpoint(NULL));
    unsigned cases = 0;
    for (unsigned invalid = 0; invalid < 17; ++invalid) {
        xhci_state_t state = fresh();
        switch (invalid) {
        case 0: state.audio_interval = 0; break;
        case 1: state.audio_interval = 17; break;
        case 2: state.device_speed = 2; state.audio_interval = 14; break;
        case 3: state.audio_endpoint = 0; break;
        case 4: state.audio_endpoint = 16; break;
        case 5: state.audio_max_packet = 0; break;
        case 6: state.audio_max_packet = PAGE_SIZE + 1; break;
        case 7: state.device_slot = 0; break;
        case 8: state.context_size = 16; break;
        case 9: state.input_context.cpu = NULL; break;
        case 10: state.output_context.cpu = NULL; break;
        case 11: state.audio_ring.page = (void *)1; break;
        case 12: state.audio_ring.cpu = (void *)1; break;
        case 13: state.audio_ring.mapping.device = (void *)1; break;
        case 14: state.audio_buffer.page = (void *)1; break;
        case 15: state.audio_buffer.cpu = (void *)1; break;
        case 16: state.audio_buffer.mapping.device = (void *)1; break;
        }
        rejected(&state); ++cases;
    }
    for (unsigned context = 32; context <= 64; context += 32)
    for (unsigned speed = 2; speed <= 3; ++speed)
    for (unsigned capture = 0; capture < 2; ++capture)
    for (unsigned high = 0; high < 2; ++high) {
        xhci_state_t state = fresh();
        state.context_size = context; state.device_speed = speed;
        state.audio_endpoint = 15; state.audio_endpoint_in = capture;
        state.audio_interval = high ? (speed == 2 ? 13 : 16) : 1;
        unsigned endpoint_id = 30 + capture;
        assert(xhci_configure_audio_endpoint(&state));
        assert(allocations == 2 && !releases && syncs == 3 && commands == 1);
        assert(state.audio_enqueue == 0 && state.audio_cycle == 1 && !state.audio_transfer_pending);
        assert(state.audio_ring.mapping.direction == DMA_BIDIRECTIONAL);
        assert(state.audio_buffer.mapping.direction == (capture ? DMA_FROM_DEVICE : DMA_TO_DEVICE));
        uint32_t *ep = (uint32_t *)(input + context * (endpoint_id + 1));
        unsigned interval = state.audio_interval - 1 + (speed == 2 ? 3 : 0);
        assert(ep[0] == interval << 16);
        assert(ep[1] == ((capture ? 5U : 1U) << 3 | 248U << 16));
        assert(ep[2] == 0x1001 && ep[3] == 0 && ep[4] == (248U | 248U << 16));
        assert(((uint32_t *)input)[1] == (1U | 1U << endpoint_id));
        xhci_trb_t *ring = state.audio_ring.cpu;
        assert(ring[255].parameter == 0x1000 && ring[255].control == ((6U << 10) | 3));
        memset(state.audio_buffer.cpu, 0x67, PAGE_SIZE);
        allocations = releases = syncs = commands = 0;
        rejected(&state);
        for (unsigned i = 0; i < PAGE_SIZE; ++i) assert(((uint8_t *)state.audio_buffer.cpu)[i] == 0x67);
        xhci_free_page(&state.audio_ring); xhci_free_page(&state.audio_buffer);
        ++cases;
    }
    for (unsigned failure = 1; failure <= 2; ++failure) {
        xhci_state_t state = fresh(); fail_allocation = failure;
        uint8_t saved[4096]; memcpy(saved, input, sizeof(input));
        assert(!xhci_configure_audio_endpoint(&state));
        assert(allocations == failure && releases == failure - 1 && !syncs && !commands);
        assert(!state.audio_ring.page && !state.audio_buffer.page);
        assert(!memcmp(saved, input, sizeof(input))); ++cases;
    }
    printf("USB 音频配置：%u 组参数、上下文大小、输入输出端点、分配失败和重复配置通过\n", cases);
}
'''
with tempfile.TemporaryDirectory(prefix='usb-audio-config-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), '-I' + str(root / 'include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
