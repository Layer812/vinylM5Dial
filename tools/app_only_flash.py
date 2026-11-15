"""Read-only target validation + app-only ESP32-S3 upload (no bootloader/partition writes).

Does not reconfigure otadata, NVS, bootloader, partitions, flash layout, or entire flash.
Rejects uncertain OTA states rather than guessing the active partition.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

FLASH_SIZE = 8 * 1024 * 1024
PART_TABLE_OFF = 0x8000
SECTOR_SIZE = 0x1000


def parse_partitions(blob):
    result=[]
    for start in range(0, len(blob), 32):
        entry=blob[start:start+32]
        if len(entry)!=32: break
        if entry[:2] == b'\xff\xff': break
        if entry[:2] == b'\xeb\xeb': continue # checksum marker
        if entry[:2] != b'\xaa\x50':
            raise ValueError(f'Unrecognized partition header at table +0x{start:x}')
        typ,sub,offset,size,label,flags=struct.unpack_from('<BBII16sI',entry,2)
        label=label.split(b'\x00',1)[0].decode('ascii',errors='replace')
        if offset % SECTOR_SIZE or size==0 or offset+size>FLASH_SIZE:
            raise ValueError(f'Invalid partition boundaries {label}')
        result.append(dict(type=typ, subtype=sub,offset=offset,size=size,label=label))
    if not result: raise ValueError('No partitions found')
    seen=[]
    for part in result:
        for other in seen:
            if max(part['offset'],other['offset']) < min(part['offset']+part['size'],other['offset']+other['size']):
                raise ValueError('Overlapping flash partitions')
        seen.append(part)
    return result


def choose_boot_app(parts, ota_data=None):
    apps=[p for p in parts if p['type']==0 and (p['subtype']==0 or 0x10<=p['subtype']<=0x1f)]
    factories=[p for p in apps if p['subtype']==0]
    ota=sorted([p for p in apps if 0x10<=p['subtype']<=0x1f],key=lambda p:p['subtype'])
    if len(factories)>1: raise ValueError('Multiple factory apps')
    if len(ota)>1 and [p['subtype'] for p in ota]!=list(range(0x10,0x10+len(ota))):
        raise ValueError('Noncontiguous OTA app slots')
    ota_part=[p for p in parts if p['type']==1 and p['subtype']==0]
    if len(ota_part)>1: raise ValueError('Multiple otadata partitions')
    if ota_part:
        if ota_data is None: raise ValueError('Read-only OTA selection data required')
        if len(ota_data)<SECTOR_SIZE*2: raise ValueError('Incomplete OTA selection data')
        valid=[]
        empty=[]
        for i in range(2):
            entry=ota_data[i*SECTOR_SIZE:i*SECTOR_SIZE+32]
            seq=struct.unpack_from('<I',entry)[0]
            state=struct.unpack_from('<I',entry,24)[0]
            crc=struct.unpack_from('<I',entry,28)[0]
            if entry==b'\xff'*32:
                empty.append(True)
                print(f'OTA sector {i}: erased',flush=True)
                continue
            empty.append(False)
            # ESP-IDF otatool.py uses binascii.crc32(seq_bytes, 0xFFFFFFFF):
            # normal zlib.crc32(seq_bytes) is NOT equivalent.
            expected=zlib.crc32(entry[:4], 0xffffffff) & 0xffffffff
            crc_ok = crc == expected
            print(f'OTA sector {i}: seq=0x{seq:08x} state=0x{state:08x} '
                  f'crc=0x{crc:08x} expected=0x{expected:08x} '
                  f'crc_ok={crc_ok}', flush=True)
            if seq not in (0, 0xffffffff) and crc_ok and state in (2,0xffffffff):
                valid.append((seq,i,state))
            elif seq not in (0, 0xffffffff) and crc_ok and state in (0, 1):
                raise ValueError(f'OTA image state {state} needs manual boot-state inspection')
            # INVALID (3) or ABORTED (4) is intentionally ignored; it is not bootable.

        if valid:
            if not ota: raise ValueError('OTA selected but no OTA applications')
            seq,_,_=max(valid)
            return ota[(seq-1)%len(ota)]
        if not all(empty):
            raise ValueError('Nonblank otadata with no recognizable valid OTA selector: inspect printed OTA sector values (nothing written)')
    if factories: return factories[0]
    if ota:
        raise ValueError('OTA slots exist without a valid boot selector or factory app; refuse to guess')
    raise ValueError('No usable factory/OTA app partition')


def find_esptool():
    user=Path(os.environ.get('USERPROFILE',Path.home()))
    pio=Path(os.environ.get('PLATFORMIO_CORE_DIR',user/'.platformio'))
    tool=pio/'packages'/'tool-esptoolpy'
    candidates=[tool/'esptool.py',tool/'esptool'/'__main__.py']
    for path in candidates:
        if path.is_file(): return [sys.executable,str(path)]
    raise RuntimeError(f'PlatformIO esptool was not found under: {tool}')


def run_esptool(prefix,port,cmd,after='no-reset'):
    call=prefix+['--chip','esp32s3','--port',port,'--baud','460800',
                 '--before','default-reset','--after',after]+cmd
    print('ESPTOOL:', ' '.join(map(str,call)),flush=True)
    subprocess.run(call,check=True)


def validate_firmware(path,part):
    binary=path.read_bytes()
    if not binary or binary[0]!=0xE9:
        raise ValueError('Firmware image is missing or has an invalid ESP image magic')
    if len(binary)>part['size']:
        raise ValueError(f'Firmware {len(binary)} bytes exceeds app slot {part["size"]}')
    if len(binary)<4096:
        raise ValueError('Firmware unexpectedly small')
    if part['offset']%SECTOR_SIZE:
        raise ValueError('Unaligned app offset')
    if part['offset']+len(binary)>FLASH_SIZE:
        raise ValueError('Firmware exceeds physical flash capacity')
    return len(binary)


def main(argv):
    port=argv[1] if len(argv)>1 else 'COM21'
    firmware=Path(__file__).resolve().parents[1]/'.pio'/'build'/'m5dial'/'firmware.bin'
    if not firmware.is_file():
        raise FileNotFoundError(f'Build required first: {firmware}')
    tool=find_esptool()
    with tempfile.TemporaryDirectory(prefix='vinyl_app_only_') as tmp:
        partfile=Path(tmp)/'partitions.bin'
        run_esptool(tool,port,['read-flash',hex(PART_TABLE_OFF),hex(SECTOR_SIZE),str(partfile)])
        parts=parse_partitions(partfile.read_bytes())
        otas=[p for p in parts if p['type']==1 and p['subtype']==0]
        ota_data=None
        if otas:
            if otas[0]['size']<2*SECTOR_SIZE:
                raise ValueError('otadata partition smaller than 2 sectors')
            ota_file=Path(tmp)/'otadata.bin'
            run_esptool(tool,port,['read-flash',hex(otas[0]['offset']),hex(2*SECTOR_SIZE),str(ota_file)])
            ota_data=ota_file.read_bytes()
        app=choose_boot_app(parts,ota_data)
        n=validate_firmware(firmware,app)
        print('READ-ONLY VERIFIED:',f'{app["label"]} app offset=0x{app["offset"]:x}',
              f'capacity={app["size"]}, firmware={n} bytes',flush=True)
        print('WRITE SCOPE: only active application image; NO bootloader/partition/NVS/otadata writes.',flush=True)
        run_esptool(tool,port,['write-flash','--flash-mode','keep','--flash-freq','keep',
                               '--flash-size','keep',hex(app['offset']),str(firmware)],after='hard-reset')

if __name__=='__main__':
    try: main(sys.argv)
    except (ValueError,FileNotFoundError,RuntimeError,subprocess.CalledProcessError) as exc:
        print(f'STOP (no further writes attempted): {exc}',file=sys.stderr)
        sys.exit(1)
