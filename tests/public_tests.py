#!/usr/bin/env python3
"""Pruebas publicas del TP1 FAT12.

Se ejecutan con:  make test
o bien:           python3 tests/public_tests.py ./build/fat12tool
"""
from __future__ import annotations
import hashlib
import pathlib
import subprocess
import sys
import tempfile

TOOL = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './build/fat12tool')
BASE = pathlib.Path(__file__).resolve().parent.parent
IMG = BASE / 'imagenes' / 'tp1_publica.img'
FAT16 = BASE / 'imagenes' / 'fat16_rechazo.img'

EXPECTED = {
    '/README.TXT': ('6d8babbc843a9443d3f85e346a494bdf9a304e003069b6426254ebd2935a83b0', 129),
    '/MANUAL.TXT': ('315032546bf127cd7dbf9b6b52468e10108e2bb7218527d7d3fe67eaa0e754af', 1651),
    '/RELLENO.BIN': ('b48ca7fadfa5d35c2ab03ff9efa2e4d67c4a7a56bb8b305e365020d379d75036', 1900),
    '/DOCS/REDES.TXT': ('c91873177437931a1f7fd5771e3bad8b5c851a5fa35b1364bd1b1ab28d53b103', 58),
    '/DOCS/LABS/GUIA.TXT': ('3eb7c217f24dce520fef058b50c15a31512aff5fa5b87ac7509e31cce6fca66c', 61),
}

DELETED = ('/SECRETO.TXT',
           '404fc7ee7a2915619d36b9305136ae265dd455f4a8e99c91874477a06c1480fc', 750)


def run(*args: str, expect: int = 0) -> subprocess.CompletedProcess[bytes]:
    cp = subprocess.run([str(TOOL), *map(str, args)],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if cp.returncode != expect:
        raise AssertionError(
            f"comando fallido: {TOOL} {' '.join(map(str, args))}\n"
            f"rc esperado={expect}, obtenido={cp.returncode}\n"
            f"stdout={cp.stdout.decode(errors='replace')}\n"
            f"stderr={cp.stderr.decode(errors='replace')}"
        )
    return cp


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def test_info() -> None:
    out = run('info', IMG).stdout.decode()
    required = {
        'fat_type=FAT12',
        'partition_lba=2048',
        'bytes_per_sector=512',
        'sectors_per_cluster=1',
        'sectors_per_fat=9',
        'root_entries=224',
        'cluster_count=2847',
    }
    missing = [line for line in required if line not in out]
    assert not missing, f'faltan lineas en info: {missing}'


def test_listing() -> None:
    root = run('ls', IMG, '/').stdout.decode()
    for token in ('README.TXT', 'MANUAL.TXT', 'RELLENO.BIN', 'DOCS', '?ECRETO.TXT'):
        assert token in root, f'{token} no aparece en el listado de la raiz'
    docs = run('ls', IMG, '/DOCS').stdout.decode()
    assert 'REDES.TXT' in docs and 'LABS' in docs
    labs = run('ls', IMG, '/DOCS/LABS').stdout.decode()
    assert 'GUIA.TXT' in labs


def test_read_and_extract() -> None:
    for path, (expected_hash, expected_size) in EXPECTED.items():
        data = run('cat', IMG, path).stdout
        assert len(data) == expected_size, (path, len(data), expected_size)
        assert sha(data) == expected_hash, (path, sha(data), expected_hash)
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / 'manual.bin'
        run('extract', IMG, '/MANUAL.TXT', out)
        assert sha(out.read_bytes()) == EXPECTED['/MANUAL.TXT'][0]


def test_fragmented_chain() -> None:
    """MANUAL.TXT y RELLENO.BIN tienen sus clusters intercalados en la imagen.

    Una solucion que lea clusters consecutivos en lugar de seguir la cadena de
    la FAT devuelve el tamano correcto pero el contenido equivocado. Esta prueba
    existe para que ese error no pase inadvertido.
    """
    manual = run('cat', IMG, '/MANUAL.TXT').stdout
    relleno = run('cat', IMG, '/RELLENO.BIN').stdout
    assert sha(manual) == EXPECTED['/MANUAL.TXT'][0], (
        'MANUAL.TXT no coincide: revise que la lectura siga la cadena de la FAT '
        'y no asuma clusters consecutivos'
    )
    assert sha(relleno) == EXPECTED['/RELLENO.BIN'][0], (
        'RELLENO.BIN no coincide: revise el recorrido de la cadena de clusters'
    )


def test_recovery() -> None:
    path, expected_hash, expected_size = DELETED
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / 'recuperado.bin'
        run('recover', IMG, path, out)
        data = out.read_bytes()
        assert len(data) == expected_size, (len(data), expected_size)
        assert sha(data) == expected_hash


def test_errors() -> None:
    cp = run('info', FAT16, expect=3)
    assert b'FAT12' in cp.stderr
    run('cat', IMG, '/NOEXISTE.TXT', expect=6)
    run('ls', IMG, '/README.TXT', expect=7)
    run('cat', IMG, '/DOCS', expect=8)


def test_image_is_never_modified() -> None:
    before = sha(IMG.read_bytes())
    with tempfile.TemporaryDirectory() as td:
        run('info', IMG)
        run('ls', IMG, '/')
        run('extract', IMG, '/MANUAL.TXT', pathlib.Path(td) / 'a.bin')
        run('recover', IMG, '/SECRETO.TXT', pathlib.Path(td) / 'b.bin')
    assert before == sha(IMG.read_bytes()), 'la herramienta modifico la imagen'


def main() -> None:
    tests = [test_info, test_listing, test_read_and_extract, test_fragmented_chain,
             test_recovery, test_errors, test_image_is_never_modified]
    for test in tests:
        test()
        print(f'[OK] {test.__name__}')
    print(f'PUBLIC TESTS PASSED: {len(tests)}/{len(tests)}')


if __name__ == '__main__':
    main()
