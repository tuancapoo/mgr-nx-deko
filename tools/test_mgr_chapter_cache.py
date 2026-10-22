"""Format/integrity tests for chapter capture and portable pipeline merging."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

import mgr_chapter_cache as cache
import mgr_scan_shader_containers as scanner


def record(vs=1, ps=2, vs_fp=111, ps_fp=222, spec=0):
    result = bytearray(cache.RECORD_BYTES)
    struct.pack_into('<II', result, 0, vs, ps)
    struct.pack_into('<I', result, 20, spec)
    struct.pack_into('<QQII', result, 80, vs_fp, ps_fp, 1, 1)
    struct.pack_into('<IIII', result, 104, 0, 0, 109, 0)
    struct.pack_into('<I', result, 360, 12)
    return bytes(result)


def container():
    data = bytearray(100)
    struct.pack_into('>III', data, 0, 0x102a1101, 88, 12)
    struct.pack_into('>I', data, 24, 28)
    struct.pack_into('>II', data, 28, 0, 12)
    return bytes(data)


def combined(records, driver=b'opaque local driver data'):
    data = cache.make_list(records)
    return struct.pack('<4sIQQ', b'NFPC', 1, len(data), len(driver)) + data + driver


class ChapterCacheTests(unittest.TestCase):
    def test_layout_and_corruption(self):
        good = cache.make_list([record()])
        self.assertEqual(cache.read_list(good), [record()])
        for bad in (good[:-1], good + b'X', good[:4] + struct.pack('<I', 99) + good[8:],
                    struct.pack('<4sIII', b'NFPL', 2, 424, 65537), b'NFPL'):
            with self.assertRaises(ValueError):
                cache.read_list(bad)
        for bad in (combined([record()])[:-1], b'NFPC' + b'\0' * 20):
            with self.assertRaises(ValueError):
                cache.read_pipeline_file(bad)
        invalid = bytearray(record())
        struct.pack_into('<I', invalid, 100, 0)  # attributes reference a missing binding
        with self.assertRaises(ValueError):
            cache.validate_record(invalid)

    def test_stable_fingerprints(self):
        self.assertEqual(cache.canonical_record(record()), cache.canonical_record(record(vs=800, ps=999)))
        self.assertNotEqual(cache.canonical_record(record()), cache.canonical_record(record(vs_fp=333)))
        self.assertNotEqual(cache.canonical_record(record()), cache.canonical_record(record(spec=2)))
        self.assertFalse(cache.validate_record(record(vs_fp=0)))

    def test_chapter_roundtrip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, store = root/'game', root/'store'
            (game/'collect/containers').mkdir(parents=True)
            (game/'collect/variants').mkdir()
            (game/'cache').mkdir()
            (game/'userdata').mkdir()
            (game/'userdata/private-save').write_bytes(b'exclude me')
            (game/'collect/containers/vs_one.bin').write_bytes(container())
            (game/'collect/variants/vs_complete.bin.pending').write_bytes(container())
            (game/'collect/variants/vs_incomplete.bin.pending').write_bytes(b'bad')
            (game/'cache/sa2_native_pipelines.bin').write_bytes(combined([record()]))
            snapshot = root/'r00.zip'
            cache.export_snapshot(game, 'R-00', snapshot)
            with zipfile.ZipFile(snapshot) as archive:
                self.assertNotIn('userdata/private-save', archive.namelist())
            first = cache.merge_snapshot(snapshot, 'R-00', store)
            self.assertEqual((first['new_assets'], first['new_pipelines']), (2, 1))
            self.assertEqual(len(first['rejected']), 1)
            self.assertGreater(first['driver_cache_bytes_excluded'], 0)
            self.assertTrue(cache.merge_snapshot(snapshot, 'R-00', store)['already_imported'])
            # Same shaders now have different package IDs; one new state is added.
            (game/'cache/sa2_native_pipelines.bin').write_bytes(combined([
                record(vs=80, ps=90), record(spec=2)]))
            second = cache.merge_snapshot(game, 'R-01', store)
            self.assertEqual((second['new_assets'], second['new_pipelines']), (0, 1))
            seed = root/'shared/mgr_pipeline_seed.bin'
            cache.export_seed(store, seed)
            records = cache.read_list(seed.read_bytes())
            self.assertEqual(len(records), 2)
            self.assertNotIn(b'opaque local driver data', seed.read_bytes())
            self.assertEqual(len(cache.read_list((game/'cache/sa2_native_pipelines.bin').read_bytes()[24:-24])), 2)
            # Repeated seed generation is deterministic and refuses conflicting output.
            cache.export_seed(store, seed)
            seed.write_bytes(b'keep me')
            with self.assertRaises(ValueError):
                cache.export_seed(store, seed)
            self.assertEqual(seed.read_bytes(), b'keep me')

    def test_snapshot_integrity_and_zip_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            broken = root/'broken.zip'
            with zipfile.ZipFile(broken, 'w') as archive:
                archive.writestr('collect/containers/vs_one.bin', container())
                archive.writestr('snapshot.json', json.dumps({'chapter': 'R-00', 'files': {
                    'collect/containers/vs_one.bin': {'bytes': 100, 'sha256': 'wrong'}}}))
            with self.assertRaises(ValueError):
                cache.merge_snapshot(broken, 'R-00', root/'store')
            self.assertFalse((root/'store/catalog.json').exists())
            malicious = root/'malicious.zip'
            with zipfile.ZipFile(malicious, 'w') as archive:
                archive.writestr('../collect/vs_x.bin', container())
            with self.assertRaises(ValueError):
                list(cache.snapshot_files(malicious))

    def test_no_silent_overflow(self):
        with self.assertRaises(ValueError):
            cache.make_list([record()] * (cache.MAX_RECORDS + 1))

    def test_bounded_static_scan(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root/'test.cpk'
            bad = struct.pack('>III', 0x102a1101, 0xffffffff, 0xffffffff)
            good = container()
            game.write_bytes(b'CPK test' + bad + good + good + good[:40])
            report = scanner.scan([game], root/'scan')
            self.assertEqual(report['unique_containers'], 1)
            self.assertEqual(report['new_review_inputs'], 1)
            self.assertEqual(report['containers'][0]['occurrences'], 2)
            self.assertFalse(report['compressed_or_encrypted_data_decoded'])


if __name__ == '__main__':
    unittest.main()
