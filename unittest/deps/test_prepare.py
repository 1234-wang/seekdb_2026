# Copyright (c) 2025 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Offline checks for dependency transport, archive integrity and ready caches."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("seekdb_prepare", ROOT / "deps/init/prepare.py")
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class Response(io.BytesIO):
    def __init__(self, data, url="https://example.invalid/pkg.rpm"):
        super().__init__(data)
        self.url = url

    def geturl(self):
        return self.url


class PrepareTest(unittest.TestCase):
    URL = "https://example.invalid/pkg.rpm"
    BYTES = b"trusted package bytes"
    DIGEST = hashlib.sha256(BYTES).hexdigest()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.profile = self.root / "profile.deps"
        self.profile.write_text("[target-default]\nrepo=https://example.invalid\n[deps]\npkg.rpm\n")
        self.checksums = self.root / "checksums.json"
        self.checksums.write_text(json.dumps({self.URL: self.DIGEST}))
        patcher = mock.patch.object(prepare, "RPM_CHECKSUMS", self.checksums)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.destination = self.root / "sdk"
        self.cache = self.root / "archives"
        self.cache.mkdir()

    def opener(self, data, url=None):
        opener = mock.Mock()
        opener.open.return_value = Response(data, url or self.URL)
        return mock.patch.object(prepare.urllib.request, "build_opener", return_value=opener)

    def test_https_required_before_download(self):
        with mock.patch.object(prepare.urllib.request, "build_opener") as opener:
            for url in ("http://example.invalid/pkg.rpm", "file:///tmp/pkg.rpm", "https:/missing-host"):
                with self.subTest(url=url), self.assertRaises(ValueError):
                    prepare.download(url, self.root / "pkg.rpm", self.DIGEST)
            opener.assert_not_called()

    def test_redirect_must_stay_https(self):
        handler = prepare.HTTPSOnlyRedirectHandler()
        request = urllib.request.Request(self.URL)
        for target in ("http://example.invalid/pkg.rpm", "file:///tmp/pkg.rpm"):
            with self.subTest(target=target), self.assertRaises(ValueError):
                handler.redirect_request(request, None, 302, "Found", {}, target)
        redirected = handler.redirect_request(request, None, 302, "Found", {},
                                              "https://other.invalid/pkg.rpm")
        self.assertEqual("https://other.invalid/pkg.rpm", redirected.full_url)

    def test_final_download_url_must_be_https(self):
        archive = self.root / "pkg.rpm"
        with self.opener(self.BYTES, "http://example.invalid/pkg.rpm"), self.assertRaises(ValueError):
            prepare.download(self.URL, archive, self.DIGEST)
        self.assertFalse(archive.exists())
        self.assertFalse(archive.with_name("pkg.rpm.download").exists())

    def test_valid_download_is_promoted_only_after_verification(self):
        archive = self.root / "pkg.rpm"
        with self.opener(self.BYTES):
            prepare.download(self.URL, archive, self.DIGEST)
        self.assertEqual(self.BYTES, archive.read_bytes())
        self.assertFalse(archive.with_name("pkg.rpm.download").exists())

    def test_tampered_download_is_not_unpacked_or_marked_ready(self):
        with self.opener(b"tampered"), mock.patch.object(prepare, "unpack") as unpack:
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                prepare.populate(self.profile, self.destination, "new-key", self.cache)
            unpack.assert_not_called()
        self.assertFalse(self.destination.exists())
        self.assertEqual([], list(self.root.glob(".deps-*")))

    def test_tampered_archive_cache_is_not_unpacked_or_marked_ready(self):
        (self.cache / "pkg.rpm").write_bytes(b"tampered")
        with mock.patch.object(prepare, "unpack") as unpack:
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                prepare.populate(self.profile, self.destination, "new-key", self.cache)
            unpack.assert_not_called()
        self.assertFalse(self.destination.exists())

    def test_valid_archive_cache_is_checked_and_unpacked(self):
        (self.cache / "pkg.rpm").write_bytes(self.BYTES)
        with mock.patch.object(prepare, "unpack") as unpack, mock.patch.object(prepare, "download") as download:
            prepare.populate(self.profile, self.destination, "new-key", self.cache)
            unpack.assert_called_once()
            download.assert_not_called()
        self.assertTrue(prepare.ready(self.destination, "new-key"))

    def test_unpinned_rpm_fails_before_ready_or_network(self):
        self.checksums.write_text("{}")
        self.destination.mkdir()
        (self.destination / "DONE").touch()
        (self.destination / hashlib.md5(self.profile.read_bytes()).hexdigest()).touch()
        with mock.patch.object(prepare, "populate") as populate:
            with self.assertRaisesRegex(ValueError, "No pinned SHA-256"):
                prepare.install(self.profile, self.destination)
            populate.assert_not_called()
        with mock.patch.object(prepare.urllib.request, "build_opener") as opener:
            with self.assertRaisesRegex(ValueError, "No pinned SHA-256"):
                prepare.download(self.URL, self.root / "pkg.rpm")
            opener.assert_not_called()

    def test_invalid_rpm_digest_is_rejected(self):
        for digest in ("", "xyz", "0" * 63, 17):
            self.checksums.write_text(json.dumps({self.URL: digest}))
            with self.subTest(digest=digest), self.assertRaisesRegex(ValueError, "Invalid pinned SHA-256"):
                prepare.verified_packages(self.profile)

    def test_http_profile_is_rejected(self):
        self.profile.write_text(self.profile.read_text().replace("https://", "http://"))
        with self.assertRaisesRegex(ValueError, "must use HTTPS"):
            prepare.verified_packages(self.profile)

    def test_non_rpm_archives_remain_https_only_without_claiming_a_pin(self):
        self.profile.write_text(self.profile.read_text().replace("pkg.rpm", "pkg.tar.gz"))
        self.assertEqual([("pkg.tar.gz", "https://example.invalid/pkg.tar.gz", None)],
                         prepare.verified_packages(self.profile))

    def test_old_ready_cache_does_not_bypass_verification(self):
        self.destination.mkdir()
        (self.destination / "DONE").touch()
        (self.destination / hashlib.md5(self.profile.read_bytes()).hexdigest()).touch()
        with mock.patch.object(prepare, "populate") as populate:
            prepare.install(self.profile, self.destination)
            populate.assert_called_once()

    def test_only_relevant_digest_changes_invalidate_ready_cache(self):
        self.destination.mkdir()
        (self.destination / "pkg").mkdir()
        (self.destination / "pkg/pkg.rpm").write_bytes(self.BYTES)
        with mock.patch.object(prepare, "unpack"):
            prepare.install(self.profile, self.destination)
        first_key = next(p.name for p in self.destination.iterdir() if len(p.name) == 64)
        self.checksums.write_text(json.dumps({self.URL: self.DIGEST, "https://else.invalid/other.rpm": "1" * 64}))
        with mock.patch.object(prepare, "populate") as populate:
            prepare.install(self.profile, self.destination)
            populate.assert_not_called()
        self.checksums.write_text(json.dumps({self.URL: "2" * 64}))
        with mock.patch.object(prepare, "populate") as populate:
            prepare.install(self.profile, self.destination)
            populate.assert_called_once()
            self.assertNotEqual(first_key, populate.call_args.args[2])

    @unittest.skipIf(os.name == "nt", "shared cache uses Unix flock")
    def test_shared_cache_identity_changes_with_pinned_digest(self):
        shared = self.root / "shared"

        def stage(profile, destination, key, archive_directory, entries):
            destination.mkdir(parents=True)
            (destination / key).touch()
            (destination / "DONE").touch()

        with mock.patch.object(prepare, "populate", side_effect=stage):
            prepare.install(self.profile, self.destination, shared)
        first_target = self.destination.resolve()
        self.checksums.write_text(json.dumps({self.URL: "2" * 64}))
        with mock.patch.object(prepare, "populate", side_effect=stage) as populate:
            prepare.install(self.profile, self.destination, shared)
            populate.assert_called_once()
        self.assertNotEqual(first_target, self.destination.resolve())


class RepositoryProfilesTest(unittest.TestCase):
    def test_all_profiles_use_https_and_every_rpm_has_a_pin(self):
        profiles = list(prepare.PROFILES.glob("*.deps"))
        self.assertTrue(profiles)
        for profile in profiles:
            with self.subTest(profile=profile.name):
                entries = prepare.verified_packages(profile)
                self.assertTrue(entries)
                for filename, url, digest in entries:
                    self.assertEqual("https", urllib.parse.urlsplit(url).scheme)
                    if filename.endswith(".rpm"):
                        self.assertRegex(digest, r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
