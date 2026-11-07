#!/usr/bin/env python3
"""Build a deterministic SF2 containing only MIDI-used presets and their samples."""
import argparse, collections, hashlib, json, struct, subprocess, zipfile, zlib
from pathlib import Path

def midi_events(data):
    if data[:4]!=b'MThd': raise ValueError('Not MIDI')
    tracks=struct.unpack_from('>H',data,10)[0]; pos=8+struct.unpack_from('>I',data,4)[0]; events=[]
    for track in range(tracks):
        length=struct.unpack_from('>I',data,pos+4)[0]; q=pos+8; end=q+length; pos=end; tick=0; running=0
        def varlen():
            nonlocal q
            value=0
            while True:
                b=data[q];q+=1;value=(value<<7)|(b&127)
                if not b&128:return value
        while q<end:
            tick+=varlen(); status=data[q]
            if status&128:q+=1
            else:status=running
            if status==255:
                kind=data[q];q+=1;n=varlen();q+=n
                if kind==47:break
            elif status in (240,247):q+=varlen()
            elif 128<=status<240:
                running=status;kind=status&240;n=1 if kind in (192,208) else 2
                args=data[q:q+n];q+=n;events.append((tick,len(events),status,args))
            else:raise ValueError('Invalid MIDI status')
    return sorted(events)

def audit(jars):
    used=collections.defaultdict(set); songs={}; banks=set()
    for jar in jars:
        with zipfile.ZipFile(jar) as z:
            for name in sorted(z.namelist()):
                if not name.startswith('snd/') or not name.endswith('.mid'):continue
                data=z.read(name); programs=[0]*16;bank=[0]*16;song_used=collections.defaultdict(set)
                for tick,order,status,args in midi_events(data):
                    ch=status&15;kind=status&240
                    if kind==192:programs[ch]=args[0]
                    elif kind==176 and args[0]==0:bank[ch]=32768|args[1]
                    elif kind==176 and args[0]==32:bank[ch]=((bank[ch]&127)<<7 if bank[ch]&32768 else 0)|args[1]
                    elif kind==176 and args[0]==121:bank[ch]=0
                    elif kind==144 and args[1]:
                        b=(bank[ch]&32767)|(128 if ch==9 else 0);p=programs[ch]
                        used[b,p].add(args[0]);song_used[b,p].add(args[0]);banks.add(b)
                songs[f'{Path(jar).name}:{name}']={'sha256':hashlib.sha256(data).hexdigest(),'presets':{f'{b}:{p}':sorted(notes) for (b,p),notes in sorted(song_used.items())}}
    return used,songs

def chunks(data,start=0,end=None):
    end=len(data) if end is None else end
    while start+8<=end:
        kind=data[start:start+4];n=struct.unpack_from('<I',data,start+4)[0];body=data[start+8:start+8+n]
        if len(body)!=n:raise ValueError('Truncated RIFF chunk')
        yield kind,body;start+=8+n+(n&1)

def pack_chunk(kind,body):return kind+struct.pack('<I',len(body))+body+(b'\0' if len(body)&1 else b'')
def records(data,n):
    if len(data)%n:raise ValueError('Invalid SF2 record length')
    return [data[i:i+n] for i in range(0,len(data),n)]

def subset(source,used,max_rate=22050,ffmpeg=None):
    raw=source.read_bytes()
    if raw[:4]!=b'RIFF' or raw[8:12]!=b'sfbk':raise ValueError('Not SF2')
    lists={v[:4]:v[4:] for k,v in chunks(raw,12) if k==b'LIST'}
    pd=dict(chunks(lists[b'pdta']));sd=dict(chunks(lists[b'sdta']))
    ph=records(pd[b'phdr'],38);pb=[struct.unpack('<HH',b) for b in records(pd[b'pbag'],4)]
    pg=[struct.unpack('<HH',b) for b in records(pd[b'pgen'],4)];pm=records(pd[b'pmod'],10)
    ins=records(pd[b'inst'],22);ib=[struct.unpack('<HH',b) for b in records(pd[b'ibag'],4)]
    ig=[struct.unpack('<HH',b) for b in records(pd[b'igen'],4)];im=records(pd[b'imod'],10)
    sh=records(pd[b'shdr'],46)
    def name(b):return b[:20].split(b'\0')[0].decode('latin1')
    presets={struct.unpack_from('<HH',b,20)[::-1]:i for i,b in enumerate(ph[:-1])}
    missing=set(used)-set(presets)
    if missing:raise ValueError(f'Missing SF2 presets: {sorted(missing)}')
    chosen=sorted(presets[k] for k in used); needed_inst=set()
    for p in chosen:
        for bag in range(struct.unpack_from('<H',ph[p],24)[0],struct.unpack_from('<H',ph[p+1],24)[0]):
            needed_inst.update(value for op,value in pg[pb[bag][0]:pb[bag+1][0]] if op==41)
    # Keep only instrument sample zones that can be selected by notes in these MIDIs.
    inst_notes=collections.defaultdict(set)
    for p in chosen:
        bank,program=struct.unpack_from('<HH',ph[p],20)[::-1]
        for bag in range(struct.unpack_from('<H',ph[p],24)[0],struct.unpack_from('<H',ph[p+1],24)[0]):
            for op,value in pg[pb[bag][0]:pb[bag+1][0]]:
                if op==41:inst_notes[value].update(used[bank,program])
    inst_ids=sorted(needed_inst);inst_map={old:new for new,old in enumerate(inst_ids)};samples=set()
    keep_ibags=set()
    for i in inst_ids:
        for bag in range(struct.unpack_from('<H',ins[i],20)[0],struct.unpack_from('<H',ins[i+1],20)[0]):
            zone=ig[ib[bag][0]:ib[bag+1][0]]
            sample=[value for op,value in zone if op==53]
            lo,hi=0,127
            for op,value in zone:
                if op==43:lo,hi=value&255,value>>8
            if sample and not any(lo<=note<=hi for note in inst_notes[i]):continue
            keep_ibags.add(bag);samples.update(sample)
    pending=list(samples)
    while pending:
        old=pending.pop();link,kind=struct.unpack_from('<HH',sh[old],42)
        if kind&32768:raise ValueError('ROM samples unsupported')
        if kind&14 and link not in samples:samples.add(link);pending.append(link)
    sample_ids=sorted(samples);sample_map={old:new for new,old in enumerate(sample_ids)}
    sample_bytes=bytearray();new_sh=[]
    for old in sample_ids:
        record=bytearray(sh[old]);start,end,loop0,loop1,rate=struct.unpack_from('<IIIII',record,20)
        if not 0<=start<end<=len(sd[b'smpl'])//2:raise ValueError('Sample bounds invalid')
        pcm=sd[b'smpl'][start*2:end*2];new_rate=min(rate,max_rate) if max_rate else rate
        if new_rate!=rate:
            if not ffmpeg:raise ValueError('--ffmpeg is required for sample-rate conversion')
            pcm=subprocess.run([str(ffmpeg),'-hide_banner','-loglevel','error','-f','s16le','-ar',str(rate),'-ac','1','-i','pipe:0','-f','s16le','-ar',str(new_rate),'-ac','1','pipe:1'],input=pcm,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout
        count=len(pcm)//2;base=len(sample_bytes)//2;sample_bytes.extend(pcm);sample_bytes.extend(bytes(92))
        def relocate(point):return base+max(0,min(count,round((point-start)*new_rate/rate)))
        struct.pack_into('<IIIII',record,20,base,base+count,relocate(loop0),relocate(loop1),new_rate)
        link,kind=struct.unpack_from('<HH',record,42)
        struct.pack_into('<H',record,42,sample_map[link] if kind&14 else 0);new_sh.append(bytes(record))
    new_sh.append(b'EOS'+bytes(17)+struct.pack('<IIIIIBbHH',len(sample_bytes)//2,len(sample_bytes)//2,0,0,0,0,0,0,1))
    def rebuild(ids,headers,bag_offset,bags,gens,mods,ref_op,remap,terminal_name,keep_bags=None):
        new_headers=[];new_bags=[];new_gens=[];new_mods=[]
        for old in ids:
            header=bytearray(headers[old]);struct.pack_into('<H',header,bag_offset,len(new_bags));new_headers.append(bytes(header))
            first=struct.unpack_from('<H',headers[old],bag_offset)[0];last=struct.unpack_from('<H',headers[old+1],bag_offset)[0]
            for b in range(first,last):
                if keep_bags is not None and b not in keep_bags:continue
                new_bags.append(struct.pack('<HH',len(new_gens),len(new_mods)))
                new_gens.extend(struct.pack('<HH',op,remap[value] if op==ref_op else value) for op,value in gens[bags[b][0]:bags[b+1][0]])
                new_mods.extend(mods[bags[b][1]:bags[b+1][1]])
        header=bytearray(len(headers[0]));header[:len(terminal_name)]=terminal_name;struct.pack_into('<H',header,bag_offset,len(new_bags));new_headers.append(bytes(header))
        new_bags.append(struct.pack('<HH',len(new_gens),len(new_mods)));new_gens.append(bytes(4));new_mods.append(bytes(10))
        return b''.join(new_headers),b''.join(new_bags),b''.join(new_gens),b''.join(new_mods)
    ph2,pb2,pg2,pm2=rebuild(chosen,ph,24,pb,pg,pm,41,inst_map,b'EOP')
    in2,ib2,ig2,im2=rebuild(inst_ids,ins,20,ib,ig,im,53,sample_map,b'EOI',keep_ibags)
    new_pd=b''.join(pack_chunk(k,v) for k,v in [(b'phdr',ph2),(b'pbag',pb2),(b'pmod',pm2),(b'pgen',pg2),(b'inst',in2),(b'ibag',ib2),(b'imod',im2),(b'igen',ig2),(b'shdr',b''.join(new_sh))])
    body=b'sfbk'+pack_chunk(b'LIST',b'INFO'+lists[b'INFO'])+pack_chunk(b'LIST',b'sdta'+pack_chunk(b'smpl',bytes(sample_bytes)))+pack_chunk(b'LIST',b'pdta'+new_pd)
    result=pack_chunk(b'RIFF',body)
    report={'source_bytes':len(raw),'subset_bytes':len(result),'presets':[{'bank':struct.unpack_from('<H',ph[p],22)[0],'program_zero_based':struct.unpack_from('<H',ph[p],20)[0],'name':name(ph[p])} for p in chosen],'instruments':len(inst_ids),'samples':len(sample_ids),'max_sample_rate':max_rate,'sample_pcm_bytes':len(sample_bytes),'decoded_float_sample_bytes':len(sample_bytes)*2,'sf2_info':{k.decode('ascii'):v.rstrip(b'\0').decode('latin1') for k,v in chunks(lists[b'INFO']) if k in (b'INAM',b'ICOP',b'ICMT')}}
    return result,report

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--jar',action='append',default=[]);ap.add_argument('--used',type=Path,help='bank:program -> notes list (cpp/dist/soundfont/used-notes.json) instead of --jar');ap.add_argument('--save-used',type=Path,help='write the bank:program -> notes list audited from --jar');ap.add_argument('--source',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--sample-rate',type=int,default=22050);ap.add_argument('--ffmpeg',type=Path);args=ap.parse_args()
    if bool(args.jar)==bool(args.used):ap.error('give either --jar (one or more) or --used')
    if args.used:
        used=collections.defaultdict(set,{tuple(int(v) for v in key.split(':')):set(notes) for key,notes in json.loads(args.used.read_text(encoding='utf-8')).items()});songs={}
    else:used,songs=audit(args.jar)
    if args.save_used:args.save_used.write_text(json.dumps({f'{b}:{p}':sorted(notes) for (b,p),notes in sorted(used.items())},indent=1)+'\n',encoding='utf-8',newline='\n')
    data,report=subset(args.source,used,args.sample_rate,args.ffmpeg);report['midi']=songs;report['source_sha256']=hashlib.sha256(args.source.read_bytes()).hexdigest();report['subset_sha256']=hashlib.sha256(data).hexdigest()
    compressed=zlib.compress(data,9);report['compressed_bytes']=len(compressed)
    notes=sorted((bank,program,note) for (bank,program),keys in used.items() for note in keys)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    (args.output.parent/'used-notes.bin').write_bytes(b''.join(struct.pack('<HHB',*item) for item in notes))
    if args.jar:
        fixtures=args.output.parent/'snd';fixtures.mkdir(exist_ok=True)
        with zipfile.ZipFile(args.jar[0]) as archive:
            for name in sorted(archive.namelist()):
                if name.startswith('snd/') and name.endswith('.mid'):(fixtures/Path(name).name).write_bytes(archive.read(name))
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_bytes(data);args.output.with_suffix('.sf2.zlib').write_bytes(compressed);args.output.with_suffix('.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('midi','sf2_info')},ensure_ascii=False,indent=2))
if __name__=='__main__':main()
