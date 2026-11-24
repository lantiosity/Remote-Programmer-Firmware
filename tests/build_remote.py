"""Build the simulated DAP dispatcher for native and cross-repository tests."""
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build_remote(output: Path) -> ctypes.CDLL:
    gcc = shutil.which('gcc')
    if not gcc:
        raise SystemExit('Native gcc required (e.g. MSYS2 UCRT64 on Windows).')
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (cjson / 'cJSON.c').exists():
        raise SystemExit('Run idf.py reconfigure first to resolve the locked cJSON dependency.')

    output.mkdir(parents=True, exist_ok=True)
    backend = (ROOT / 'tests/native/remote_backend.c').read_text(encoding='utf-8')
    header = (ROOT / 'main/include/probe_core.h').read_text(encoding='utf-8')
    stubs = []
    for result, name, args in re.findall(r'\b(esp_err_t|probe_status_t|bool|uint32_t|uint8_t|void)\s+(probe_\w+)\(([^;{}]*)\)\s*;', header):
        if not re.search(r'\b' + name + r'\s*\(', backend):
            stubs.append(f'{result} {name}({args}) {{' + ('' if result == 'void' else ' return 0;') + ' }')
    backend_path = output / 'remote_backend.c'
    backend_path.write_text(backend + '\n' + '\n'.join(stubs), encoding='utf-8')
    library = output / ('remote.dll' if os.name == 'nt' else 'remote.so')
    names = ['dap_server.c', 'dap_cmsis.c', 'dap_session.c', 'target_flash_h743.c']
    subprocess.run([gcc, '-shared', '-fPIC', *(['-Wl,--export-all-symbols'] if os.name == 'nt' else []),
                    '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-I' + str(ROOT / 'tests/native/include'), '-I' + str(ROOT / 'main/include'),
                    '-I' + str(ROOT / 'main'), '-I' + str(cjson),
                    *(str(ROOT / 'main' / name) for name in names),
                    str(cjson / 'cJSON.c'), str(backend_path), '-lm', '-o', str(library)], check=True)
    if os.name == 'nt':
        with os.add_dll_directory(str(Path(gcc).parent)):
            lib = ctypes.CDLL(str(library))
    else:
        lib = ctypes.CDLL(str(library))
    lib.test_link_request.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
    lib.test_link_request.restype = ctypes.c_size_t
    return lib
