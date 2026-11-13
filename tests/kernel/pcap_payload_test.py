"""Regression: padded TCP ACKs must not appear as network payload."""
import pathlib
import struct
import json
import subprocess
import tempfile

source = (pathlib.Path(__file__).resolve().parents[2] /
          'tools/chromium/pcap-startup-summary.py').read_text()
scope = {'struct': struct}
exec(source[source.index('def tcp_payload('):source.index('root=')], scope)
decode = scope['tcp_payload']

def packet(data=b'', fragment=0, src=7890, dst=49227, seq=0, ack=0, flags=0):
    ip = bytearray(20)
    ip[0] = 0x45
    ip[9] = 6
    ip[2:4] = struct.pack('!H', 40 + len(data))
    ip[6:8] = struct.pack('!H', fragment)
    tcp = bytearray(20)
    tcp[:12] = struct.pack('!HHII', src, dst, seq, ack)
    tcp[12] = 0x50
    tcp[13] = flags
    frame = bytes(12) + b'\x08\x00' + ip + tcp + data
    return frame + bytes(max(0, 60 - len(frame)))

assert decode(packet()) == (7890, 49227, b'', 0, 0, 0)
assert decode(packet(b'abc')) == (7890, 49227, b'abc', 0, 0, 0)
assert decode(packet(b'abc', 0x2000)) is None
assert decode(packet(b'abc', 1)) is None
assert decode(packet(b'abc')[:50]) is None
bad = bytearray(packet()); bad[46] = 0x40
assert decode(bad) is None
assert decode(b'') is None
print('TCP ACK padding, payload lengths, fragments and truncated headers passed')

raw_track = scope['track_ack']
state = {}
def track(*args):
    return raw_track(*args, state)
waiting = {}
assert track(waiting, True, b'abc', 100, 0, 0x18, 1) == []
assert track(waiting, True, b'abc', 100, 0, 0x18, 4) == []
assert track(waiting, False, b'', 0, 102, 0x10, 5) == []
assert track(waiting, False, b'', 0, 103, 0x10, 6) == [5]
assert not waiting
state.clear() # A distinct test connection for sequence wrap.
track(waiting, True, b'abc', 0xfffffffe, 0, 0x18, 10)
assert track(waiting, False, b'', 0, 0xffffffff, 0x10, 11) == []
assert track(waiting, False, b'', 0, 1, 0x10, 12) == [2]
track(waiting, True, b'abc', 100, 0, 0x18, 15)
assert track(waiting, False, b'', 0, 103, 0, 16) == []
assert track(waiting, False, b'data', 0, 103, 0x18, 17) == [2]
print('ACK timing: retransmissions, partial ACK, wrap and piggyback ACK passed')

# ACK 之后到达的重复数据包不得开启新的延迟采样。
# 否则下一个普通 ACK 可能被误报为客户机长时间停顿。
waiting = {}
state.clear()
track(waiting, True, b'abc', 100, 0, 0x18, 1)
assert track(waiting, False, b'', 0, 103, 0x10, 2) == [1]
track(waiting, True, b'abc', 100, 0, 0x18, 3)
assert track(waiting, False, b'', 0, 103, 0x10, 60) == []
assert not waiting
print('Post-ACK duplicates do not create false long-stall samples')

# 针对持久连接运行真实 CLI，然后重用其端口。
# 最近时间窗报告必须包括较早建立的连接上的流量，但不能继承该连接旧的延迟样本或
# FIN/RST 状态。
records = []
def record(stamp, incoming, data=b'', seq=0, ack=0, flags=0x10):
    frame = packet(data, src=7890 if incoming else 50000,
                   dst=50000 if incoming else 7890, seq=seq, ack=ack, flags=flags)
    seconds = int(stamp)
    records.append(struct.pack('<IIII', seconds, round((stamp-seconds)*1000000),
                               len(frame), len(frame)) + frame)

request = b'CONNECT test.example:443 HTTP/1.1\r\n\r\n'
record(9, False, seq=99, flags=2)
record(9.1, True, seq=1000, ack=100, flags=0x12)
record(9.2, False, seq=100, ack=1001)
record(10, False, request, seq=100, ack=1001, flags=0x18)
record(11, True, b'abc', seq=1001, flags=0x18)
record(12, False, ack=1004)
record(13, True, b'abc', seq=1001, flags=0x18)
record(60, False, ack=1004)
record(61, True, b'def', seq=1004, flags=0x18)
record(61.5, False, ack=1007)
record(62, True, seq=1007, flags=0x11)
record(63, False, ack=1008, flags=0x11)
record(100, False, seq=200, flags=2)
record(100.1, True, seq=5000, ack=201, flags=0x12)
record(100.2, False, seq=201, ack=5001)
record(101, False, request, seq=201, ack=5001, flags=0x18)
record(102, True, b'xyz', seq=5001, flags=0x18)
record(102.25, False, ack=5004)
record(103, True, seq=5004, flags=0x14)
header = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65536, 1)
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='liteos-pcap-interval-') as directory:
    path = pathlib.Path(directory)
    capture = path/'input.pcap'
    report = path/'report.json'
    capture.write_bytes(header+b''.join(records)+b'partial')
    command = ['python3', str(root/'tools/chromium/pcap-startup-summary.py'),
               '--host', 'test.example', '--capture-file', str(capture),
               '--output', str(report)]
    subprocess.run(command, capture_output=True, check=True)
    full = json.loads(report.read_text())
    assert len(full) == 2 and [row['connection'] for row in full] == [1, 2]
    assert full[0]['max_guest_ack_delay_seconds'] == 1
    assert full[1]['max_guest_ack_delay_seconds'] == .25
    assert full[0]['close_events'][0]['fin']
    assert full[1]['close_events'][0]['rst']
    assert 'request_utc' not in full[0]
    subprocess.run(command+['--last-seconds', '45'], capture_output=True, check=True)
    recent = json.loads(report.read_text())
    assert len(recent) == 2 and recent[0]['max_guest_ack_delay_seconds'] == .5
    assert recent[0]['window_start_capture_timestamp_seconds'] == 58
    subprocess.run(command+['--last-seconds', '1'], capture_output=True, check=True)
    recent = json.loads(report.read_text())
    assert len(recent) == 1 and recent[0]['connection'] == 2
    assert recent[0]['max_guest_ack_delay_seconds'] == .25
    idle = bytearray(packet()); idle[23] = 17 # Non-TCP record advances capture clock.
    capture.write_bytes(header+b''.join(records)+
                        struct.pack('<IIII', 200, 0, len(idle), len(idle))+idle)
    result = subprocess.run(command+['--last-seconds', '1'], capture_output=True,
                            text=True, check=True)
    assert json.loads(report.read_text()) == []
    assert 'not a successful request test' in result.stdout
    for invalid in ['0', '-1', 'nan', 'inf']:
        assert subprocess.run(command+['--last-seconds', invalid],
                              capture_output=True).returncode == 2
print('PCAP CLI: recent intervals, persistent flows, port reuse, FIN/RST, partial tails and invalid input passed')
