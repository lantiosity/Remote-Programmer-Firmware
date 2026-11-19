"""Package an existing source release and matching ESP-IDF build, without flashing.

Usage from firmware/: python tools/package-images.py 0.1.1
Requires the esptool installed by ESP-IDF. Never overwrites release images/ZIP.
"""
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import zipfile
from esptool.bin_image import LoadFirmwareImage

root = Path(__file__).resolve().parents[1]
version = sys.argv[1]
if not re.fullmatch(r'\d+\.\d+\.\d+', version):
    raise ValueError('Expected major.minor.patch version')
release, build = root/'release'/version, root/'build'
images, archive = release/'images', release/f'firmware-{version}-flash.zip'
if images.exists() or archive.exists():
    raise FileExistsError('Existing release images must not be overwritten')
read_json = lambda p: json.loads(p.read_text(encoding='utf-8'))
sha = lambda data: hashlib.sha256(data).hexdigest()
source = read_json(release/'manifest.json')
source_zip = release/source['archive']
assert source['version'] == version and sha(source_zip.read_bytes()) == source['archive_sha256']
project, args = read_json(build/'project_description.json'), read_json(build/'flasher_args.json')
assert project['project_version'] == version and project['target'] == 'esp32s3'
assert args['flash_settings'] == {'flash_mode':'dio','flash_size':'16MB','flash_freq':'80m'}
expected = {'bootloader/bootloader.bin':0,'partition_table/partition-table.bin':0x8000,
            'esp_swdlink.bin':0x10000,'ota_data_initial.bin':0x310000}
assert {name:int(offset,16) for offset,name in args['flash_files'].items()} == expected
flash_args = shlex.split((build/'flash_args').read_text(encoding='utf-8'))
assert dict(zip(flash_args[:6:2],flash_args[1:6:2])) == {
    '--flash-mode':'dio','--flash-freq':'80m','--flash-size':'16MB'}
assert {name:int(offset,16) for offset,name in zip(flash_args[6::2],flash_args[7::2])} == expected
git = ['git','-C',str(root)]
assert not subprocess.check_output(git+['diff','HEAD','--','.'])
commit = subprocess.check_output(git+['rev-parse','HEAD'],text=True).strip()
for entry in source['files']:
    assert sha(subprocess.check_output(git+['show',f"HEAD:{entry['path']}"])) == entry['sha256'],entry['path']

# Validate chip, app description and SDK checksums before publishing anything.
for name in ['esp_swdlink.bin','bootloader/bootloader.bin']:
    image = LoadFirmwareImage('esp32s3',str(build/name))
    image.verify()
    assert image.stored_digest == image.calc_digest
app = (build/'esp_swdlink.bin').read_bytes()
assert struct.unpack_from('<H',app,12)[0] == 9 and app[32:36] == b'\x32\x54\xcd\xab'
assert app[48:80].split(b'\0')[0].decode() == version
assert app[80:112].split(b'\0')[0] == b'esp_swdlink'
partitions, md5_found = [],False
table = (build/'partition_table/partition-table.bin').read_bytes()
for pos in range(0,len(table),32):
    magic = struct.unpack_from('<H',table,pos)[0]
    if magic == 0xffff: break
    if magic == 0xebeb:
        assert hashlib.md5(table[:pos]).digest() == table[pos+16:pos+32]
        md5_found = True
        break
    assert magic == 0x50aa
    _,typ,subtype,offset,size,label,flags = struct.unpack_from('<HBBII16sI',table,pos)
    partitions.append({'name':label.split(b'\0')[0].decode(),'offset':offset,'size':size})
assert md5_found
assert [(p['name'],p['offset'],p['size']) for p in partitions] == [
    ('nvs',0x9000,0x6000),('phy_init',0xf000,0x1000),('factory',0x10000,0x300000),
    ('otadata',0x310000,0x2000),('ota_0',0x320000,0x400000),('ota_1',0x720000,0x400000),
    ('offline',0xb20000,0x4e0000)]
assert len(app) <= 0x300000
assert (build/'ota_data_initial.bin').read_bytes() == b'\xff'*8192
for name,offset in expected.items():
    end = offset+(build/name).stat().st_size
    for part in partitions:
        if part['name'] in ['nvs','phy_init','offline']:
            assert end <= part['offset'] or offset >= part['offset']+part['size']

images.mkdir()
for name in expected:
    target = images/name
    target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(build/name,target)
shutil.copyfile(build/'flash_args',images/'flash_args')
shutil.copyfile(root/'partitions.csv',images/'partitions.csv')
shutil.copyfile(root/'docs/FRONT_PANEL.md',images/'FRONT_PANEL.md')
(images/'README.md').write_text((root/'tools/flash-readme.md.in').read_text(encoding='utf-8').replace('@VERSION@',version),encoding='utf-8')
config = {}
for line in (root/'sdkconfig').read_text(encoding='utf-8').splitlines():
    if line.startswith(('CONFIG_SPIRAM_','CONFIG_ESPTOOLPY_FLASH','CONFIG_BT_NIMBLE_MAX_CONNECTIONS=',
                        'CONFIG_BLE_OTA_REQUIRE_ENCRYPTION=','CONFIG_OTA_','CONFIG_LINK_OTA_TCP_PORT=',
                        'CONFIG_FRONT_PANEL_','CONFIG_FRONT_LCD_')) and '=' in line:
        key,value = line.split('=',1)
        config[key] = value
manifest = {'project':'esp_swdlink','version':version,'kind':'flash-images','source_commit':commit,
            'source_archive':source_zip.name,'source_archive_sha256':source['archive_sha256'],
            'esp_idf':project['git_revision'],'hardware':'ESP32-S3 N16R8','flash_settings':args['flash_settings'],
            'build_config':config,'partitions':partitions,
            'validation':{'image_checksum_digest':'passed','partition_md5_and_bounds':'passed',
                          'source_archive_matches_commit':'passed','hardware_flash':'not performed'},
            'flash_files':[{'path':name,'offset':hex(offset)} for name,offset in expected.items()],
            'files':[]}
for p in sorted(images.rglob('*')):
    if p.is_file(): manifest['files'].append({'path':p.relative_to(images).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p.read_bytes())})
(images/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
(images/'SHA256SUMS.txt').write_text(''.join(f'{sha(p.read_bytes())}  {p.relative_to(images).as_posix()}\n' for p in sorted(images.rglob('*')) if p.is_file()),encoding='utf-8')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for p in sorted(images.rglob('*')):
        if p.is_file():
            info = zipfile.ZipInfo(p.relative_to(images).as_posix())
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            z.writestr(info,p.read_bytes())
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    for name in z.namelist(): assert z.read(name) == (images/name).read_bytes(),name
(release/'SHA256SUMS.txt').write_text(''.join(f'{sha(p.read_bytes())}  {p.name}\n' for p in [source_zip,archive]),encoding='utf-8')
(release/'README.md').write_text(f'''# 固件 {version} 发布包

- `{source_zip.name}`：源码及文档，清单见 `manifest.json`。
- `{archive.name}`：解压后直接烧录，包含四个 BIN、flash_args、README 与校验和。
- [images/README.md](images/README.md)：原生 USB 烧录地址与 OTA 步骤；OTA 只选 `images/esp_swdlink.bin`。

适用 ESP32-S3 N16R8；factory 3 MiB，两个 OTA 槽各 4 MiB，offline 预留 4.875 MiB。
来源提交 `{commit}`；版本标签 v{version} 在发布包提交后创建。已有版本包保持不变。
已完成本机构建及原生协议/内存/串口回归；本次未烧写物理设备，BLE/USB/WiFi 与高速 SPI2 实测待验收。
''',encoding='utf-8')
print(f'{version}: {len(app)} app bytes, SHA256 {sha(app)}, flash ZIP {archive.stat().st_size} bytes')
