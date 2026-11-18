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
                '-I' + str(ROOT / 'main/include'), '-I' + str(ROOT / 'main'), '-I' + str(CJSON),
                str(ROOT / 'main/dap_server.c'), str(ROOT / 'main/dap_cmsis.c'),
                str(ROOT / 'main/dap_session.c'), str(CJSON / 'cJSON.c'),
                str(BUILD / 'remote_backend.c'), '-lm', '-o', str(library)], check=True)
if os.name == 'nt':
    _dll_path = os.add_dll_directory(str(Path(GCC).parent))
lib = ctypes.CDLL(str(library))
lib.test_remote_process.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
lib.test_remote_process.restype = ctypes.c_size_t
lib.test_link_request.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
lib.test_link_request.restype = ctypes.c_size_t
lib.test_ota_window.argtypes = [ctypes.c_int64, ctypes.c_int64, ctypes.c_uint, ctypes.c_uint]
lib.test_ota_window.restype = ctypes.c_bool

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

config_library = BUILD / ('config.dll' if os.name == 'nt' else 'config.so')
subprocess.run([GCC, '-shared', '-fPIC', *(['-Wl,--export-all-symbols'] if os.name == 'nt' else []),
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-I' + str(ROOT / 'tests/native/include'), '-I' + str(ROOT / 'main/include'),
                str(ROOT / 'main/app_cfg.c'), str(ROOT / 'tests/native/config_backend.c'),
                '-o', str(config_library)], check=True)
config = ctypes.CDLL(str(config_library))
config.cfg_wifi_set.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
config.cfg_wifi_get.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
config.cfg_wifi_get.restype = ctypes.c_bool


def counter(name):
    return ctypes.c_uint.in_dll(lib, name).value


def request(name, args, ident=1, raw=None):
    text = (raw or json.dumps({'id': ident, 'request': name, 'arguments': args}, separators=(',', ':'))).encode()
    output = ctypes.create_string_buffer(20480)
    size = lib.test_remote_process(text, len(text), output, len(output))
    result = json.loads(output.raw[:size])
    assert size < len(output)
    return result


class RemoteTests(unittest.TestCase):
    def setUp(self):
        lib.dap_server_select_session(0)
        lib.dap_server_reset_handles()
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

    def test_malformed_requests_and_unsigned_arguments(self):
        for raw in ['[]','{} true','{"request":"hello","arguments":[1]} trailing']:
            self.assertNotEqual(request('',[],raw=raw)['status'],0)
        for value in [-1,2**32,1.5]:
            self.assertNotEqual(request('set_clock',[value])['status'],0)
            self.assertNotEqual(request('read_mem',[value,0,32])['status'],0)

    def test_swj_token_skips_nested_and_quoted_fields(self):
        text = '{"ignored":{"arguments":[0,1]},"text":"\\\"arguments\\\"","request":"swj_sequence","arguments":[51,72057594037927935],"id":9}'
        self.assertEqual(request('',[],raw=text)['status'],0)
        self.assertEqual(bytes((ctypes.c_uint8*8192).in_dll(lib,'test_data'))[:7],b'\xff'*7)


class LinkTests(unittest.TestCase):
    USB, TCP, BLE = range(3)

    def setUp(self):
        lib.test_link_init()

    def exchange(self, sink, data, token=1):
        output = ctypes.create_string_buffer(20480)
        size = lib.test_link_request(sink, token, data, len(data), output)
        return output.raw[:size]

    def remote(self, sink, name, args=(), token=1):
        data = json.dumps({'id': 42, 'request': name, 'arguments': args}).encode()
        return json.loads(self.exchange(sink, data, token))

    def test_active_writer_rejects_usb_and_wireless_connect_reset_and_close(self):
        self.assertEqual(self.remote(self.TCP, 'open')['status'], 0)
        handle = self.remote(self.TCP, 'get_memory_interface_for_ap', [1, 0])['result']
        self.assertEqual(self.remote(self.TCP, 'write_block8', [handle, 0, [1, 2]])['status'], 0)
        self.assertEqual(self.remote(self.BLE, 'readprop', ['unique_id'])['result'], 'native-test')
        self.assertGreater(len(self.exchange(self.USB, b'\x00\x03')), 2)  # DAP_Info
        for command in ('connect', 'reset', 'close', 'disconnect', 'lock'):
            self.assertIn('owned', self.remote(self.BLE, command)['error'])
        for command, status in [(2, 0), (3, 255), (10, 255), (17, 255)]:
            self.assertEqual(self.exchange(self.USB, bytes([command, 0]))[:2], bytes([command, status]))
        self.assertEqual(self.remote(self.TCP, 'write_block8', [handle, 2, [3, 4]])['status'], 0)
        self.assertEqual((counter('test_connects'), counter('test_resets'), counter('test_disconnects')), (1, 0, 0))
        self.assertEqual(self.remote(self.TCP, 'close')['status'], 0)
        self.assertEqual(self.exchange(self.USB, b'\x02\x01'), b'\x02\x01')
        self.assertIn('owned', self.remote(self.TCP, 'reset')['error'])

    def test_nested_remote_lock_releases_only_after_final_unlock(self):
        for _ in range(2):
            self.assertEqual(self.remote(self.BLE, 'lock')['status'], 0)
        self.assertEqual(self.remote(self.BLE, 'set_clock', [1000])['status'], 0)
        self.assertEqual(self.remote(self.BLE, 'unlock')['status'], 0)
        self.assertIn('owned', self.remote(self.TCP, 'connect')['error'])
        self.assertEqual(self.remote(self.BLE, 'unlock')['status'], 0)
        self.assertEqual(self.remote(self.TCP, 'connect')['status'], 0)
        self.assertEqual(self.remote(self.TCP, 'lock')['status'], 0)
        self.assertEqual(self.remote(self.TCP, 'unlock')['status'], 0)
        self.assertIn('owned', self.remote(self.BLE, 'connect')['error'])

    def test_disconnect_releases_bus_and_preserves_handles_until_close(self):
        self.assertEqual(self.remote(self.TCP, 'connect')['status'], 0)
        handle = self.remote(self.TCP, 'get_memory_interface_for_ap', [1, 0])['result']
        self.assertEqual(self.remote(self.TCP, 'disconnect')['status'], 0)
        self.assertEqual(self.exchange(self.USB, b'\x02\x01'), b'\x02\x01')
        self.assertEqual(self.exchange(self.USB, b'\x03'), b'\x03\x00')
        self.assertEqual(self.remote(self.TCP, 'connect')['status'], 0)
        self.assertEqual(self.remote(self.TCP, 'read_mem', [handle, 0, 32])['status'], 0)
        self.assertEqual(self.remote(self.TCP, 'close')['status'], 0)
        self.assertNotEqual(self.remote(self.TCP, 'read_mem', [handle, 0, 32])['status'], 0)
        self.assertEqual(self.exchange(self.USB, b'\x02\x01'), b'\x02\x01')

    def test_owner_released_by_generation_timeout_or_response_failure(self):
        self.assertEqual(self.remote(self.TCP, 'open')['status'], 0)
        handle = self.remote(self.TCP, 'get_memory_interface_for_ap', [1, 0])['result']
        lib.link_session_set(self.TCP, 2)
        self.assertEqual(self.exchange(self.TCP, b'{"request":"reset"}', token=1), b'')
        self.assertNotEqual(self.remote(self.TCP, 'read_mem', [handle, 0, 32], token=2)['status'], 0)
        self.assertEqual(self.remote(self.TCP, 'connect', token=2)['status'], 0)
        ctypes.c_int64.in_dll(lib, 'test_now').value += 119_000_000
        self.assertEqual(self.remote(self.TCP, 'readprop', ['unique_id'], token=2)['status'], 0)
        ctypes.c_int64.in_dll(lib, 'test_now').value += 1_000_000
        lib.test_link_poll()
        ctypes.c_bool.in_dll(lib, 'test_send_failed').value = True
        self.assertEqual(self.exchange(self.USB, b'\x02\x01'), b'\x02\x01')
        ctypes.c_bool.in_dll(lib, 'test_send_failed').value = False
        self.assertEqual(self.remote(self.BLE, 'connect')['status'], 0)
        self.assertEqual(counter('test_disconnects'), 3)


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

    def test_reset_without_nreset_reports_failure_without_writing_dp(self):
        core.test_disable_nreset()
        ctypes.c_uint.in_dll(core, 'model_fault').value = 1
        transfers = ctypes.c_uint.in_dll(core, 'model_transfers')
        before = transfers.value
        self.assertNotEqual(core.probe_reset_target(), 0)
        self.assertEqual(transfers.value, before)


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


class OtaAuthorizationTests(unittest.TestCase):
    def test_transport_masks_and_expiry(self):
        for allowed in range(8):
            for transport in [1,2,4,7]:
                with self.subTest(allowed=allowed,transport=transport):
                    self.assertEqual(lib.test_ota_window(99,100,allowed,transport),bool(allowed & transport))
                    self.assertFalse(lib.test_ota_window(100,100,allowed,transport))
                    self.assertFalse(lib.test_ota_window(101,100,allowed,transport))
        self.assertTrue(lib.test_ota_window(2**40,2**40+1,1,1))
        self.assertFalse(lib.test_ota_window(1,0,7,7))


class WifiConfigTests(unittest.TestCase):
    def setUp(self):
        config.test_cfg_reset()
        self.assertEqual(config.cfg_init(), 0)

    def flag(self, name):
        return ctypes.c_bool.in_dll(config, name)

    def count(self, name):
        return ctypes.c_uint.in_dll(config, name).value

    def credentials(self):
        ssid, password = ctypes.create_string_buffer(33), ctypes.create_string_buffer(65)
        configured = config.cfg_wifi_get(ssid, len(ssid), password, len(password))
        return (ssid.value, password.value) if configured else None

    def test_reboot_reserved_before_nvs_write_and_held_until_restart(self):
        self.flag('mock_ota_active').value = True
        self.assertNotEqual(config.cfg_wifi_set(b'new', b'new-pass', 1500), 0)
        self.assertNotEqual(config.cfg_wifi_clear(1500), 0)
        self.assertNotEqual(config.cfg_reboot_later(500), 0)
        self.assertEqual(self.count('mock_writes'), 0)
        self.flag('mock_ota_active').value = False
        self.assertEqual(config.cfg_wifi_set(b'new', b'new-pass', 1500), 0)
        for flag in ('mock_reserved_during_write', 'mock_reserved_during_timer',
                     'mock_reboot_reserved', 'mock_timer_scheduled'):
            self.assertTrue(self.flag(flag).value)
        self.assertNotEqual(config.cfg_wifi_clear(1500), 0)
        self.assertEqual(self.count('mock_writes'), 1)
        self.assertEqual(self.credentials(), (b'new', b'new-pass'))

    def test_nvs_write_and_commit_failures_preserve_the_previous_pair(self):
        for flag, error in [('mock_write_error', 0x1105), ('mock_commit_error', -1)]:
            with self.subTest(flag=flag):
                injected = ctypes.c_int.in_dll(config, flag)
                injected.value = error
                self.assertEqual(config.cfg_wifi_set(b'new', b'new-pass', 1500), error)
                self.assertEqual(self.credentials(), (b'legacy', b'legacy-pass'))
                self.assertFalse(self.flag('mock_reboot_reserved').value)
                self.assertFalse(self.flag('mock_timer_scheduled').value)
                self.assertEqual(config.cfg_init(), 0)
                self.assertEqual(self.credentials(), (b'legacy', b'legacy-pass'))
                injected.value = 0
        self.assertEqual(self.count('mock_commits'), 1)
        self.assertEqual(config.cfg_wifi_set(b'new', b'new-pass', 1500), 0)

    def test_full_byte_ssid_and_clear_survive_restart(self):
        for ssid in [b'A' * 31, b'A' * 32, ('网' * 10 + 'é').encode()]:
            with self.subTest(ssid=ssid):
                self.flag('mock_reboot_reserved').value = False
                self.assertEqual(config.cfg_wifi_set(ssid, b'new-pass', 1500), 0)
                self.assertEqual(config.cfg_init(), 0)
                self.assertEqual(self.credentials(), (ssid, b'new-pass'))
        for _ in range(2):
            self.flag('mock_reboot_reserved').value = False
            self.assertEqual(config.cfg_wifi_clear(1500), 0)
            self.assertEqual(config.cfg_init(), 0)
            self.assertIsNone(self.credentials())

    def test_timer_failure_releases_reboot_reservation(self):
        error = ctypes.c_int.in_dll(config, 'mock_timer_error')
        error.value = -1
        self.assertEqual(config.cfg_wifi_clear(1500), -1)
        self.assertFalse(self.flag('mock_reboot_reserved').value)
        self.assertFalse(self.flag('mock_timer_scheduled').value)
        error.value = 0
        self.assertEqual(config.cfg_reboot_later(500), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
