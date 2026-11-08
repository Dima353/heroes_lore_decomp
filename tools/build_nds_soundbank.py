#!/usr/bin/env python3
"""Nintendo DS instrument bank: GeneralUser GS presets used by the JAR's MIDIs, one zone per used
sample, IMA-ADPCM with word-aligned loops (DS sound hardware), plus a simple volume envelope.

Usage: build_nds_soundbank.py --source GeneralUser-GS.sf2 --used cpp/dist/soundfont/used-notes.json
                              --output soundbank.bin [--rate 16000] [--max-seconds 1.2]

Layout (little endian; the ARM9 reads it in place, the ARM7 plays the sample data from main RAM):
  'HLSB', u16 presets, u16 zones, u16 samples, u16 0, u32 data offset
  presets: u8 bank (0 / 128), u8 program, u16 first zone, u16 zone count, u16 0
  zones (24 bytes): u8 key lo, key hi, root key, pan (0..127), s16 tune cents, u16 sample,
          u16 attenuation cB, u16 attack ms, u16 hold ms, u16 decay ms, u16 sustain cB, u16 release ms, u16 0, u16 0
  samples (16 bytes): u32 data offset, u16 loop start words, u16 0, u32 total words, u16 rate Hz, u8 loop, u8 0
  data: per sample a 4-byte ADPCM header (first PCM value, step index) and 4-bit codes, low nibble first.
"""
import argparse, json, math, struct
from pathlib import Path

def chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    while start + 8 <= end:
        kind = data[start:start + 4]; n = struct.unpack_from('<I', data, start + 4)[0]
        yield kind, data[start + 8:start + 8 + n]; start += 8 + n + (n & 1)

def records(data, n): return [data[i:i + n] for i in range(0, len(data), n)]

STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
         107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
         876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
         5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
         27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]

def ds_decode_step(pcm, index, code):
    step = STEPS[index]; diff = step >> 3
    if code & 1: diff += step >> 2
    if code & 2: diff += step >> 1
    if code & 4: diff += step
    pcm = max(pcm - diff, -0x7fff) if code & 8 else min(pcm + diff, 0x7fff)
    return pcm, max(0, min(88, index + INDEX[code & 7]))

def adpcm_encode(samples):
    """Closed-loop IMA-ADPCM as the DS decodes it; the first sample goes into the header."""
    pcm = max(-0x7fff, min(0x7fff, samples[0] if samples else 0)); index = 0
    # A short look at the start picks a step index that suits the attack.
    if len(samples) > 1:
        peak = max(abs(samples[i + 1] - samples[i]) for i in range(min(len(samples) - 1, 16)))
        while index < 88 and STEPS[index] < peak: index += 1
    header = struct.pack('<hBB', pcm, index, 0)
    codes = []
    for s in samples:
        best = None
        for code in range(16):
            p, i = ds_decode_step(pcm, index, code)
            err = abs(p - s)
            if best is None or err < best[0]: best = (err, code, p, i)
        _, code, pcm, index = best; codes.append(code)
    if len(codes) & 7: codes += [0] * (8 - (len(codes) & 7))
    body = bytes(codes[i] | codes[i + 1] << 4 for i in range(0, len(codes), 2))
    return header + body

def resample(pcm, ratio, count):
    """count output samples, output[i] = input[i / ratio], box-filtered when shrinking."""
    out = []; n = len(pcm); width = max(1, int(1 / ratio))
    for i in range(count):
        x = i / ratio; a = int(x); f = x - a
        if width > 1:
            lo = max(0, a - width // 2); hi = min(n, lo + width)
            out.append(int(sum(pcm[lo:hi]) / max(1, hi - lo)))
        else:
            s0 = pcm[min(a, n - 1)]; s1 = pcm[min(a + 1, n - 1)]
            out.append(int(s0 + (s1 - s0) * f))
    return out

def short_loop(body, start, rate, max_loop, max_pre, width=48):
    """Loop of at most max_loop seconds inside the sustain: the best-matching junction among
    whole-word lengths, then a crossfade of the loop tail into the samples before the start.
    Returns the body (cut at the loop end) with the loop start (multiple of 8) appended."""
    start = max(width, min(start, int(max_pre * rate)))
    start += (8 - start % 8) % 8
    room = len(body) - start - width
    lo = max(8, int(0.12 * rate) // 8 * 8); hi = min(room, int(max_loop * rate)) // 8 * 8
    if hi < lo: hi = lo = max(8, room // 8 * 8)
    best = None
    for length in range(lo, hi + 1, 8):
        err = sum((body[start + k] - body[start + length + k]) ** 2 for k in range(-width, width, 4))
        if best is None or err < best[0]: best = (err, length)
    length = best[1]; out = body[:start + length]
    for j in range(width):  # fade the tail into what precedes the loop start
        a = (j + 1) / (width + 1); t = start + length - width + j
        out[t] = int(out[t] * (1 - a) + body[start - width + j] * a)
    out.append(start)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, required=True); ap.add_argument('--used', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True); ap.add_argument('--rate', type=int, default=11025)
    ap.add_argument('--max-seconds', type=float, default=0.8); ap.add_argument('--max-samples', type=int, default=2)
    ap.add_argument('--max-loop', type=float, default=0.3); ap.add_argument('--max-pre', type=float, default=0.4)
    args = ap.parse_args()
    used = {tuple(int(v) for v in k.split(':')): set(n) for k, n in json.loads(args.used.read_text(encoding='utf-8')).items()}
    raw = args.source.read_bytes()
    lists = {v[:4]: v[4:] for k, v in chunks(raw, 12) if k == b'LIST'}
    pd = dict(chunks(lists[b'pdta'])); smpl = dict(chunks(lists[b'sdta']))[b'smpl']
    ph = records(pd[b'phdr'], 38); pb = [struct.unpack('<HH', b) for b in records(pd[b'pbag'], 4)]
    pg = [struct.unpack('<HH', b) for b in records(pd[b'pgen'], 4)]
    ins = records(pd[b'inst'], 22); ib = [struct.unpack('<HH', b) for b in records(pd[b'ibag'], 4)]
    ig = [struct.unpack('<HH', b) for b in records(pd[b'igen'], 4)]; sh = records(pd[b'shdr'], 46)
    s16 = lambda v: v - 65536 if v >= 32768 else v
    presets = {struct.unpack_from('<HH', b, 20)[::-1]: i for i, b in enumerate(ph[:-1])}

    def zones_of(bags, gens, first, last):
        glob = {}; out = []
        for bag in range(first, last):
            z = {op: v for op, v in gens[bags[bag][0]:bags[bag + 1][0]]}
            if bag == first and not (41 in z or 53 in z): glob = z; continue
            out.append(z)
        return glob, out

    def key_range(z): v = z.get(43, 0x7f00); return v & 255, v >> 8
    def vel_range(z): v = z.get(44, 0x7f00); return v & 255, v >> 8

    def resolve(bank, program, note):
        """Generator set for (preset, note) at velocity 100: instrument values plus preset offsets."""
        p = presets[bank, program]
        pglob, pzones = zones_of(pb, pg, struct.unpack_from('<H', ph[p], 24)[0], struct.unpack_from('<H', ph[p + 1], 24)[0])
        for pz in pzones:
            lo, hi = key_range(pz); vl, vh = vel_range(pz)
            if not (lo <= note <= hi and vl <= 100 <= vh) or 41 not in pz: continue
            i = pz[41]
            iglob, izones = zones_of(ib, ig, struct.unpack_from('<H', ins[i], 20)[0], struct.unpack_from('<H', ins[i + 1], 20)[0])
            for iz in izones:
                lo2, hi2 = key_range(iz); vl2, vh2 = vel_range(iz)
                if not (lo2 <= note <= hi2 and vl2 <= 100 <= vh2) or 53 not in iz: continue
                g = dict(iglob); g.update(iz)
                pre = dict(pglob); pre.update(pz)
                for op, v in pre.items():  # additive preset generators
                    if op in (41, 43, 44, 53, 54, 57, 58): continue
                    g[op] = s16(g.get(op, 0)) + s16(v) if op not in (33, 34, 35, 36, 38) else \
                        s16(g.get(op, -12000)) + s16(v)
                return (i, iz[53], lo2, hi2), g
        return None, None

    out_presets = []; out_zones = []; sample_index = {}; sample_blobs = []; report = []
    tc = lambda g, op: 2 ** (s16(g.get(op, -12000)) / 1200) if op in g else 0.0
    for (bank, program) in sorted(used):
        notes = sorted(used[bank, program]); picks = {}
        for n in notes:
            key, g = resolve(bank, program, n)
            if key is None: continue
            picks.setdefault(key, (g, []))[1].append(n)
        # Too many distinct samples: keep the ones covering the most notes, the rest use the nearest kept zone.
        kept = sorted(picks.items(), key=lambda kv: -len(kv[1][1]))[:args.max_samples if bank == 0 else 64]
        kept.sort(key=lambda kv: min(kv[1][1]))
        first = len(out_zones)
        for idx, ((inst, sid, lo, hi), (g, ns)) in enumerate(kept):
            if bank == 0:  # melodic: widen key ranges so every used note finds a zone
                lo = 0 if idx == 0 else min(ns)
                hi = 127 if idx == len(kept) - 1 else min(kept[idx + 1][1][1]) - 1
            else: lo, hi = max(lo, min(ns)), min(hi, max(ns))
            name, start, end, l0, l1, rate, pitch, corr, link, stype = struct.unpack('<20sIIIIIBbHH', sh[sid])
            start += s16(g.get(0, 0)) + 32768 * s16(g.get(4, 0)); end += s16(g.get(1, 0)) + 32768 * s16(g.get(12, 0))
            l0 += s16(g.get(2, 0)) + 32768 * s16(g.get(45, 0)); l1 += s16(g.get(3, 0)) + 32768 * s16(g.get(50, 0))
            looped = g.get(54, 0) & 1 and l1 > l0 + 8
            skey = (sid, start, end, l0 if looped else 0, l1 if looped else 0)
            if skey not in sample_index:
                pcm = list(struct.unpack_from(f'<{end - start}h', smpl, start * 2))
                # Rate follows the root key: low strings and basses lose little at 8 kHz, bells keep more.
                new_rate = min(rate, max(8000, min(16000, int(args.rate * 2 ** ((pitch - 60) / 24)))))
                ratio = new_rate / rate
                if looped:
                    ls = l0 - start; ll = l1 - l0
                    if ll * ratio <= args.max_loop * new_rate and ls * ratio <= args.max_pre * new_rate:
                        # The SoundFont loop is short enough: keep it, stretched to whole ADPCM words.
                        new_ll = max(8, round(ll * ratio / 8) * 8)
                        ratio = new_ll / ll; new_rate = rate * ratio
                        new_ls = round(ls * ratio); pad = (8 - new_ls % 8) % 8
                        body = [0] * pad + resample(pcm, ratio, new_ls + new_ll)
                        loop_words = 1 + (new_ls + pad) // 8
                    else:
                        body = resample(pcm, ratio, round((ls + ll) * ratio))
                        body = short_loop(body, round(ls * ratio), new_rate, args.max_loop, args.max_pre)
                        loop_words = 1 + body.pop() // 8
                else:
                    count = min(int(len(pcm) * ratio), int(args.max_seconds * new_rate))
                    body = resample(pcm, ratio, count)
                    fade = min(len(body), int(new_rate * 0.15))
                    if count < int(len(pcm) * ratio):
                        for k in range(fade): body[-1 - k] = body[-1 - k] * k // fade
                    loop_words = 0
                data = adpcm_encode(body)
                sample_index[skey] = len(sample_blobs)
                sample_blobs.append((data, loop_words, round(new_rate), bool(looped), pitch, corr, name.split(b'\0')[0].decode('latin1')))
            si = sample_index[skey]; _, _, _, _, pitch, corr, sname = sample_blobs[si]
            root = g[58] if 58 in g else pitch
            tune = s16(g.get(51, 0)) * 100 + s16(g.get(52, 0)) + corr
            # SoundFont attenuation is scaled by 0.4 as in common players; the envelope in ms, sustain in cB.
            atten = max(0, int(s16(g.get(48, 0)) * 0.4))
            ms = lambda op: min(65535, int(tc(g, op) * 1000))
            pan = max(0, min(127, 64 + s16(g.get(17, 0)) * 64 // 500))
            sustain = max(0, min(1440, s16(g.get(37, 0))))
            out_zones.append(struct.pack('<BBBBhHHHHHHHHH', lo, hi, root & 127, pan, tune, si, atten,
                                         ms(34), ms(35), ms(36), sustain, max(ms(38), 20), 0, 0))
            report.append(f'{bank}:{program} keys {lo}-{hi} root {root} sample {sname} {len(sample_blobs[si][0])} B')
        out_presets.append(struct.pack('<BBHHH', bank, program, first, len(out_zones) - first, 0))

    table = b''.join(out_presets) + b''.join(out_zones)
    data_offset = 16 + len(table) + len(sample_blobs) * 16
    data_offset = (data_offset + 3) & ~3
    offs = []; blob = bytearray()
    for data, *_ in sample_blobs:
        offs.append(data_offset + len(blob)); blob += data
        while len(blob) & 3: blob.append(0)
    samples = b''.join(struct.pack('<IHHIHBB', o, lw, 0, len(d) // 4, r, lp, 0)
                       for o, (d, lw, r, lp, *_) in zip(offs, sample_blobs))
    head = b'HLSB' + struct.pack('<HHHHI', len(out_presets), len(out_zones), len(sample_blobs), 0, data_offset)
    result = head + table + samples
    result += bytes(data_offset - len(result)) + blob
    args.output.parent.mkdir(parents=True, exist_ok=True); args.output.write_bytes(result)
    args.output.with_suffix('.txt').write_text('\n'.join(report) + '\n', encoding='utf-8')
    print(f'soundbank: {len(out_presets)} presets, {len(out_zones)} zones, {len(sample_blobs)} samples, {len(result)} bytes')

if __name__ == '__main__': main()
