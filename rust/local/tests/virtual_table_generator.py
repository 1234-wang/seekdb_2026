# Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
"""Verify SQL cell types while local queries use business-owned field names."""
import pathlib
import subprocess
import sys
import tempfile
import unittest


class LocalVirtualTableGeneratorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="local-vtable-")
        root = pathlib.Path(__file__).resolve().parents[3]
        output = pathlib.Path(cls.directory.name)
        cls.addClassCleanup(cls.directory.cleanup)
        subprocess.run(
            [sys.executable, str(root / "src/share/inner_table/generate_inner_table_schema.py"),
             "--def-file", str(root / "src/share/inner_table/ob_inner_table_schema_def.py"),
             "--share-output-dir", str(output / "share"),
             "--observer-output-dir", str(output / "observer"), "--quiet"],
            check=True,
        )
        cls.source = (output / "observer/ob_all_virtual_local_tables.cpp").read_text()

    def test_unsigned_columns_preserve_sql_type(self):
        body = self.source.split("int ObAllVirtualMergeInfo::fill_cells()", 1)[1]
        body = body.split("ObAllVirtualColumnChecksumErrorInfo::", 1)[0]
        for name in ("frozen_scn", "global_broadcast_scn", "last_merged_scn"):
            with self.subTest(column=name):
                self.assertIn("cells[i].set_uint64({}_);".format(name), body)
        self.assertIn("cells[i].set_int(is_merge_error_);", body)

    def test_text_and_binary_columns_preserve_sql_type(self):
        self.assertIn("cells[i].set_varchar(visitor_);", self.source)
        self.assertIn("cells[i].set_varchar(job_status_);", self.source)
        self.assertIn("cells[i].set_varbinary(b_column_checksums_);", self.source)

    def test_queries_use_business_column_names(self):
        self.assertIn("LocalRequest::select(merge_info::schema(), {merge_info::Id, merge_info::FrozenScn", self.source)
        self.assertIn("tablet_checksum::BColumnChecksums", self.source)


if __name__ == "__main__":
    unittest.main()
