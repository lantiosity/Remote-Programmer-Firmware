"""Compile and exercise the real remote protocol implementation, without hardware.

Run: python tests/run_native.py (requires native gcc and IDF-managed cJSON).
All generated code/binaries stay in ignored build/native-tests/.
"""
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/native-tests'
BUILD.mkdir(parents=True, exist_ok=True)
GCC = shutil.which('gcc')
if not GCC:
    raise SystemExit('Native gcc required (e.g. MSYS2 UCRT64 on Windows).')
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'
if not (CJSON / 'cJSON.c').exists():
    raise SystemExit('Run idf.py reconfigure first to resolve the locked cJSON dependency.')
backend = (ROOT / 'tests/native/remote_backend.c').read_text(encoding='utf-8')
header = (ROOT / 'main/include/probe_core.h').read_text(encoding='utf-8')
stubs = []
for result, name, args in re.findall(r'\b(esp_err_t|probe_status_t|bool|uint32_t|uint8_t|void)\s+(probe_\w+)\(([^;{}]*)\)\s*;', header):
    if re.search(r'\b' + name + r'\s*\(', backend):
        continue
    stubs.append(f'{result} {name}({args}) {{' + ('' if result == 'void' else ' return 0;') + ' }')
(BUILD / 'remote_backend.c').write_text(backend + '\n' + '\n'.join(stubs), encoding='utf-8')
library = BUILD / ('remote.dll' if os.name == 'nt' else 'remote.so')
subprocess.run([GCC, '-shared', '-fPIC', *(['-Wl,--export-all-symbols'] if os.name == 'nt' else []), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-I' + str(ROOT / 'tests/native/include'),
                '-I' + str(ROOT / 'main/include'), '-I' + str(CJSON),
                str(ROOT / 'main/dap_server.c'), str(CJSON / 'cJSON.c'),
                str(BUILD / 'remote_backend.c'), '-lm', '-o', str(library)], check=True)
if os.name == 'nt':
    _dll_path = os.add_dll_directory(str(Path(GCC).parent))
lib = ctypes.CDLL(str(library))
lib.dap_server_process.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
lib.dap_server_process.restype = ctypes.c_size_t

# Compile the real MEM-AP implementation against a posted-read / 1 KiB TAR
# model. This detects corruption that an implementation-shaped memcpy mock
# would miss, including reads/writes crossing the auto-increment boundary.
swd_backend = (ROOT / 'tests/native/swd_backend.c').read_text(encoding='utf-8')
swd_header = (ROOT / 'main/include/probe_swd.h').read_text(encoding='utf-8')
swd_stubs = []
for result, name, args in re.findall(r'\b(esp_err_t|bool|int|uint32_t|uint8_t|void)\s+(probe_\w+)\(([^;{}]*)\)\s*;', swd_header):
    if re.search(r'\b' + name + r'\s*\(', swd_backend):
        continue
    swd_stubs.append(f'{result} {name}({args}) {{' + ('' if result == 'void' else ' return 0;') + ' }')
(BUILD / 'swd_backend.c').write_text(swd_backend + '\n' + '\n'.join(swd_stubs), encoding='utf-8')
core_library = BUILD / ('core.dll' if os.name == 'nt' else 'core.so')
subprocess.run([GCC, '-shared', '-fPIC', *(['-Wl,--export-all-symbols'] if os.name == 'nt' else []),
                '-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                '-I'+str(ROOT/'tests/native/include'),'-I'+str(ROOT/'main/include'),
                str(ROOT/'main/probe_core.c'),str(BUILD/'swd_backend.c'),'-o',str(core_library)], check=True)
core = ctypes.CDLL(str(core_library))
serial_library = BUILD / ('serial.dll' if os.name == 'nt' else 'serial.so')
subprocess.run([GCC, '-shared', '-fPIC', *(['-Wl,--export-all-symbols'] if os.name == 'nt' else []),
                '-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                '-I'+str(ROOT/'tests/native/include'),'-I'+str(ROOT/'main/include'),
                str(ROOT/'main/serial_bridge.c'),str(ROOT/'tests/native/serial_backend.c'),
                '-o',str(serial_library)], check=True)
serial = ctypes.CDLL(str(serial_library))


def counter(name):
    return ctypes.c_uint.in_dll(lib, name).value


def request(name, args, ident=1, raw=None):
    text = (raw or json.dumps({'id': ident, 'request': name, 'arguments': args}, separators=(',', ':'))).encode()
    output = ctypes.create_string_buffer(20480)
    size = lib.dap_server_process(text, len(text), output, len(output))
    result = json.loads(output.raw[:size])
    assert size < len(output)
    return result


class RemoteTests(unittest.TestCase):
    def setUp(self):
        lib.dap_server_select_session(0)
        lib.dap_server_reset_session()
        ctypes.c_uint.in_dll(lib, 'test_calls').value = 0
        self.handle = request('get_memory_interface_for_ap', [1, 7])['result']

    def test_full_page_write(self):
        for count in [2048, 4096, 5104]:
            data = [i % 256 for i in range(count)]
            self.assertEqual(request('write_block8', [self.handle, 0x20000000, data])['status'], 0)
            self.assertEqual(counter('test_count'), count)
            actual = bytes((ctypes.c_uint8 * 8192).in_dll(lib, 'test_data'))
            self.assertEqual(actual[:count], bytes(data))

    def test_invalid_byte_arrays_never_write(self):
        for data in [[0] * 5105, [256], [-1], [1.5], ['1'], [None], []]:
            before = counter('test_calls')
            self.assertNotEqual(request('write_block8', [self.handle, 0, data])['status'], 0)
            self.assertEqual(counter('test_calls'), before)

    def test_word_arrays_reject_overflow(self):
        for data in [[0] * 2049, [-1], [2**32], [1.1], ['3']]:
            self.assertNotEqual(request('write_block32', [self.handle, 0, data])['status'], 0)
            self.assertNotEqual(request('write_ap_multiple', [0, data])['status'], 0)

    def test_ap_selection_and_reuse(self):
        self.assertEqual(request('read_mem', [self.handle, 0, 32])['status'], 0)
        self.assertEqual(counter('test_apsel'), 7)
        for _ in range(10):
            self.assertEqual(request('get_memory_interface_for_ap', [1, 7])['result'], self.handle)
        for args in [[2, 0], [1, 256], [1, -1]]:
            self.assertNotEqual(request('get_memory_interface_for_ap', args)['status'], 0)

    def test_sessions_are_isolated_and_handles_reclaimed(self):
        lib.dap_server_select_session(1)
        lib.dap_server_reset_handles()
        ble = request('get_memory_interface_for_ap',[1,23])['result']
        request('read_mem',[ble,0,32])
        self.assertEqual(counter('test_apsel'),23)
        lib.dap_server_select_session(0)
        request('read_mem',[self.handle,0,32])
        self.assertEqual(counter('test_apsel'),7)
        lib.dap_server_select_session(1)
        for _ in range(10):
            lib.dap_server_reset_handles()
            self.assertEqual(request('get_memory_interface_for_ap',[1,23])['status'],0)

    def test_largest_decimal_responses(self):
        for name, limit, value in [('read_block8', 5104, 255), ('read_block32', 1856, 2**32-1), ('read_ap_multiple',1856,0)]:
            args = [0, limit] if name == 'read_ap_multiple' else [self.handle, 0, limit]
            reply = request(name, args, -2147483648)
            self.assertEqual(reply['status'], 0)
            self.assertEqual(len(reply['result']), limit)
            args[-1] += 1
            self.assertNotEqual(request(name, args)['status'], 0)

    def test_exact_swj_integer(self):
        for count, bits in [(51,2**56-1), (64,2**64-1), (136,0x1A019BC0EA2E3DDAFE986852D956209F392), (256,2**256-1)]:
            self.assertEqual(request('swj_sequence', [count,bits])['status'], 0)
            self.assertEqual(counter('test_bits'), count)
            actual = bytes((ctypes.c_uint8 * 8192).in_dll(lib,'test_data'))
            self.assertEqual(int.from_bytes(actual[:(count+7)//8], 'little'), bits & ((1<<(((count+7)//8)*8))-1))
        self.assertEqual(request('swj_sequence',[64,'0xffffffffffffffff'])['status'],0)

    def test_invalid_swj_does_not_clock(self):
        for args in [[0,1],[257,1],[51,-1],[64,1.25],[64,2**256],[1,'xyz']]:
            before = counter('test_calls')
            self.assertNotEqual(request('swj_sequence',args)['status'],0)
            self.assertEqual(counter('test_calls'),before)

    def test_swj_token_skips_nested_and_quoted_fields(self):
        text = '{"ignored":{"arguments":[0,1]},"text":"\\\"arguments\\\"","request":"swj_sequence","arguments":[51,72057594037927935],"id":9}'
        self.assertEqual(request('',[],raw=text)['status'],0)
        self.assertEqual(bytes((ctypes.c_uint8*8192).in_dll(lib,'test_data'))[:7],b'\xff'*7)


class MemoryTests(unittest.TestCase):
    def setUp(self):
        self.memory = (ctypes.c_uint8 * 16384).in_dll(core, 'model_memory')
        self.memory[:] = bytes(i % 251 for i in range(16384))
        ctypes.c_uint.in_dll(core,'model_fault').value = 0
        ctypes.c_uint.in_dll(core,'model_transfers').value = 0
        ctypes.c_uint32.in_dll(core,'model_csw').value = 0x23000052
        core.probe_disconnect()
        self.assertEqual(core.probe_mem_select_ap(0,0),0)

    def test_unaligned_page_round_trip_and_neighbours(self):
        for offset in [0,1,2,3,1021,1022,1023,1024]:
            for size in [1,2,3,4,5,4096]:
                original = bytes(self.memory)
                payload = bytes((i * 17) % 256 for i in range(size))
                data = (ctypes.c_uint8 * size).from_buffer_copy(payload)
                self.assertEqual(core.probe_mem_write_block8(offset,data,size),0)
                out = (ctypes.c_uint8 * size)()
                self.assertEqual(core.probe_mem_read_block8(offset,out,size),0)
                self.assertEqual(bytes(out),payload)
                self.assertEqual(bytes(self.memory[:offset]),original[:offset])
                self.assertEqual(bytes(self.memory[offset+size:]),original[offset+size:])
                self.assertEqual(ctypes.c_uint32.in_dll(core,'model_csw').value & 0xffffffc0,0x23000040)

    def test_word_transfer_crosses_tar_boundary(self):
        data = (ctypes.c_uint32 * 1024)(*(i * 12347 for i in range(1024)))
        self.assertEqual(core.probe_mem_write_block32(1020,data,1024),0)
        out = (ctypes.c_uint32 * 1024)()
        self.assertEqual(core.probe_mem_read_block32(1020,out,1024),0)
        self.assertEqual(list(data),list(out))

    def test_errors_and_address_overflow(self):
        data = (ctypes.c_uint32 * 4)()
        self.assertNotEqual(core.probe_mem_read_block32(1,data,1),0)
        self.assertNotEqual(core.probe_mem_write_block32(-4,data,2),0)
        self.assertNotEqual(core.probe_mem_read_block8(-1,data,2),0)
        ctypes.c_uint.in_dll(core,'model_fault').value = 1
        self.assertNotEqual(core.probe_mem_read_block32(0,data,4),0)

    def test_large_read_uses_batched_transactions(self):
        data = (ctypes.c_uint8 * 4096)()
        self.assertEqual(core.probe_mem_read_block8(0,data,4096),0)
        self.assertEqual(bytes(data),bytes(self.memory[:4096]))
        self.assertLess(ctypes.c_uint.in_dll(core,'model_transfers').value,1200)


class SerialTests(unittest.TestCase):
    def test_duplicate_subscribe_unsubscribe_and_full_table(self):
        self.assertNotEqual(serial.test_subscribe(1),0)  # init owns mutex creation
        self.assertEqual(serial.serial_bridge_init(),0)
        for _ in range(100):
            self.assertEqual(serial.test_subscribe(1),0)
            self.assertEqual(serial.test_subscribe(1),0)
            serial.test_unsubscribe(1)
            serial.test_unsubscribe(1)
            self.assertEqual(ctypes.c_uint.in_dll(serial,'mock_locks_held').value,0)
        for token in range(4): self.assertEqual(serial.test_subscribe(token),0)
        self.assertNotEqual(serial.test_subscribe(4),0)
        serial.test_unsubscribe(2)
        self.assertEqual(serial.test_subscribe(4),0)
        for token in range(5): serial.test_unsubscribe(token)
        self.assertEqual(ctypes.c_uint.in_dll(serial,'mock_locks_held').value,0)
        self.assertEqual(ctypes.c_uint.in_dll(serial,'mock_lock_errors').value,0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
