"""Report selected host proxy connection timings without exposing payloads."""
import argparse
import json
import pathlib
import struct
import math

def tcp_payload(packet):
    """Return TCP ports and data, excluding Ethernet padding/fragments."""
    if len(packet) < 34 or packet[12:14] != b'\x08\x00':
        return None
    ip = packet[14:]
    ihl = (ip[0] & 15) * 4
    total = struct.unpack('!H', ip[2:4])[0]
    fragments = struct.unpack('!H', ip[6:8])[0]
    if (ip[0] >> 4 != 4 or ihl < 20 or ip[9] != 6 or
            fragments & 0x3fff or total < ihl + 20 or total > len(ip)):
        return None
    tcp = ip[ihl:total]
    offset = (tcp[12] >> 4) * 4
    if offset < 20 or offset > len(tcp):
        return None
    src, dst = struct.unpack('!HH', tcp[:4])
    sequence, acknowledgement = struct.unpack('!II', tcp[4:12])
    return src, dst, tcp[offset:], sequence, acknowledgement, tcp[13]

def track_ack(waiting, incoming, payload, sequence, acknowledgement, flags, stamp,
              state, events=None):
    """Measure first arrival to cumulative ACK, including sequence wrap."""
    if incoming and payload:
        endpoint=(sequence+len(payload)) & 0xffffffff
        cursor = state.get('acknowledged')
        if cursor is not None and ((cursor-endpoint) & 0xffffffff) < 0x80000000:
            return []  # Already acknowledged duplicate, not a new wait.
        waiting.setdefault(endpoint,stamp)
    elif not incoming and flags & 0x10:
        cursor = state.get('acknowledged')
        if cursor is None or ((acknowledgement-cursor) & 0xffffffff) < 0x80000000:
            state['acknowledged'] = acknowledgement
        completed=[endpoint for endpoint in waiting
                   if ((acknowledgement-endpoint) & 0xffffffff) < 0x80000000]
        delays = []
        for endpoint in completed:
            first_seen = waiting.pop(endpoint)
            delay = stamp-first_seen
            delays.append(delay)
            if events is not None:
                events.append({'first_seen': first_seen, 'acked_at': stamp,
                               'delay_seconds': delay})
        return delays
    return []

root=pathlib.Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--host', default='example.com')
parser.add_argument('--capture-file', type=pathlib.Path,
                    default=root/'build/chrome-network.pcap')
parser.add_argument('--output', type=pathlib.Path,
                    help='Optional report destination (default: canonical build directory)')
parser.add_argument('--last-seconds', type=float,
                    help='Limit samples to the latest capture-clock interval')
args=parser.parse_args()
host=args.host.lower()
if (not host or len(host)>253 or
        any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789.-' for c in host)):
    parser.error('host must be an ASCII DNS name')
if args.last_seconds is not None and (not math.isfinite(args.last_seconds) or
                                      args.last_seconds <= 0):
    parser.error('last-seconds must be a finite positive number')
connect_prefix=('CONNECT '+host+':').encode('ascii')
get_prefix=('GET http://'+host+'/').encode('ascii')
flows={}
matched={}
pending={}
ack_events={}
ack_state={}
active={}
syn_sequences={}
connection_counts={}
latest_stamp=0
previous_stamp=None
timestamp_reversals=0
with args.capture_file.open('rb') as capture:
    header=capture.read(24)
    endian='<' if header[:4]==b'\xd4\xc3\xb2\xa1' else '>'
    assert struct.unpack(endian+'I',header[:4])[0]==0xa1b2c3d4
    assert struct.unpack(endian+'I',header[20:24])[0]==1
    while record:=capture.read(16):
        if len(record)!=16: break
        seconds,micros,length,_=struct.unpack(endian+'IIII',record)
        packet=capture.read(length)
        if len(packet)!=length: break
        stamp=seconds+micros/1000000
        latest_stamp=max(latest_stamp,stamp)
        if previous_stamp is not None and stamp < previous_stamp:
            timestamp_reversals+=1
        previous_stamp=stamp
        decoded=tcp_payload(packet)
        if decoded is None: continue
        src,dst,payload,sequence,acknowledgement,flags=decoded
        if src!=7890 and dst!=7890: continue
        incoming=src==7890
        port=dst if incoming else src
        key=active.get(port)
        new_syn=not incoming and flags & 0x02 and not flags & 0x10
        if key is None or (new_syn and syn_sequences.get(key)!=sequence):
            connection_counts[port]=connection_counts.get(port,0)+1
            key=(port,connection_counts[port])
            active[port]=key
            syn_sequences[key]=sequence if new_syn else None
        flows.setdefault(key,[]).append((stamp,incoming,len(payload),flags))
        # 为每个序列终点保留首次到达时间：重传不能掩盖客户机确认原始数据所用的时间。
        waiting=pending.setdefault(key,{})
        track_ack(waiting,incoming,payload,sequence,acknowledgement,flags,stamp,
                  ack_state.setdefault(key,{}),ack_events.setdefault(key,[]))
        if not incoming and (payload.startswith(connect_prefix) or
                             payload.startswith(get_prefix)):
            matched.setdefault(key,stamp)
results=[]
cutoff=latest_stamp-args.last_seconds if args.last_seconds is not None else 0
for key,start in matched.items():
    window_start=max(start,cutoff)
    packets=[p for p in flows[key] if p[0]>=window_start]
    if not packets: continue
    incoming=[p for p in packets if p[1] and p[2]]
    outgoing=[p for p in packets if not p[1] and p[2]]
    samples=[event for event in ack_events.get(key,[]) if event['first_seen']>=window_start]
    delays=[event['delay_seconds'] for event in samples if event['delay_seconds']>=0]
    # QEMU net/dump.c 使用 QEMU_CLOCK_VIRTUAL 和基于 RTC 的 start_ts。
    # 不要将其描述为宿主机墙上时钟或纯浏览器延迟。
    results.append({'port':key[0],'connection':key[1],
                    'capture_clock':'QEMU virtual clock plus RTC start, not host UTC',
                    'connect_capture_timestamp_seconds':start,
                    'window_start_capture_timestamp_seconds':window_start,
                    'window_end_capture_timestamp_seconds':latest_stamp,
                    'capture_timestamp_reversals':timestamp_reversals,
                    'first_incoming_seconds':incoming[0][0]-start if incoming else None,
                    'last_incoming_seconds':incoming[-1][0]-start if incoming else None,
                    'incoming_bytes':sum(p[2] for p in incoming),
                    'outgoing_bytes':sum(p[2] for p in outgoing),
                    'max_guest_ack_delay_seconds':max(delays,default=None),
                    'ack_samples':len(samples),
                    'negative_ack_samples':sum(event['delay_seconds']<0 for event in samples),
                    'unacknowledged_payload_endpoints':sum(
                        stamp>=window_start for stamp in pending.get(key,{}).values()),
                    'close_events':[{'seconds':round(t-start,4),
                                     'direction':'peer' if inc else 'guest',
                                     'fin':bool(flags&1),'rst':bool(flags&4)}
                                    for t,inc,n,flags in packets if flags&5],
                    'slow_ack_events':[{'first_seen_seconds':round(event['first_seen']-start,4),
                                        'acked_seconds':round(event['acked_at']-start,4),
                                        'delay_seconds':event['delay_seconds']}
                                       for event in samples if event['delay_seconds']>1],
                    'payload_timeline':[{'seconds':round(t-start,4),'direction':'in' if inc else 'out','bytes':n}
                                        for t,inc,n,flags in packets if n]})
if not matched:
    parser.error('No identifiable HTTP/CONNECT request for '+host+' captured')
label='example' if host=='example.com' else host.replace('.', '-')
output=args.output or root/f'build/chrome-{label}-network-timing.json'
output.write_text(json.dumps(results,indent=2)+'\n')
if not results:
    print(json.dumps({'host':host,'packets_in_selected_interval':0,
                      'verification':'No traffic in selected interval; not a successful request test'}))
for result in results[-3:]:
    # 长连接 HTTP/2 可能包含数千个数据包。将完整时间线放入报告，而不是输出到终端或工具响应。
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ('payload_timeline','slow_ack_events')}))
