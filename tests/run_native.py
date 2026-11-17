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


if __name__ == '__main__':
    unittest.main(verbosity=2)
