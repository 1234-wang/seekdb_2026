#!/usr/bin/env python3
"""Check that SDK packaging retains dependencies without exporting main/hooks."""
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[2] / 'tools/cmake/package_engine_sdk.py'
spec = importlib.util.spec_from_file_location('package_engine_sdk', MODULE)
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


class PackageTest(unittest.TestCase):
    def test_link_closure_and_response_file(self):
        with tempfile.TemporaryDirectory(prefix='seekdb sdk ') as directory:
            root = pathlib.Path(directory)
            (root / 'libengine.a').touch()
            (root / 'libdependency.a').touch()
            (root / 'libmalloc_hook.a').touch()
            (root / 'objects.rsp').write_text('libengine.a libmalloc_hook.a CMakeFiles/ob_main.dir/main.cpp.o')
            inputs, system = sdk.collect('c++ @objects.rsp -L. -ldependency -ldl -lpthread -ldl', root)
            self.assertEqual([root / 'libengine.a', root / 'libdependency.a'], inputs)
            self.assertEqual(['dl', 'pthread'], system)

    def test_missing_third_party_dependency_is_not_silently_exported(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                sdk.collect('c++ -lmissingthirdparty', pathlib.Path(directory))

    def test_mri_creates_flat_archive_with_quoted_paths(self):
        with tempfile.TemporaryDirectory(prefix='seekdb sdk ') as directory:
            root = pathlib.Path(directory)
            (root / 'engine.c').write_text('int sdk_test_symbol(void) { return 42; }')
            subprocess.run(['cc', '-c', str(root / 'engine.c'), '-o', str(root / 'engine.o')], check=True)
            subprocess.run(['ar', 'rcs', str(root / 'input.a'), str(root / 'engine.o')], check=True)
            link_dir = root / 'CMakeFiles/seekdb.dir'
            link_dir.mkdir(parents=True)
            (link_dir / 'link.txt').write_text('c++ input.a -lpthread')
            header = root / 'seekdb_engine.h'
            header.write_text('int sdk_test_symbol(void);')
            output = root / 'export'
            subprocess.run(['python3', str(MODULE), '--link-command', str(link_dir / 'link.txt'),
                            '--ar', 'ar', '--header', str(header), '--output', str(output)], check=True)
            archive = output / 'lib/liboceanbase_static.a'
            symbols = subprocess.check_output(['nm', str(archive)], text=True)
            self.assertIn('sdk_test_symbol', symbols)
            (root / 'host.c').write_text('int sdk_test_symbol(void); int main(void) { return sdk_test_symbol() != 42; }')
            subprocess.run(['cc', str(root / 'host.c'), str(archive), '-o', str(root / 'host')], check=True)
            subprocess.run([str(root / 'host')], check=True)


if __name__ == '__main__':
    unittest.main()
