"""Offline tests for the GitHub-release firmware fetcher.

No test here touches the network: every call injects a fake opener that
serves bytes from an in-memory map. These tests pin the safety rules that
matter — downloads only from the project's own GitHub, HTTPS only, and a
SHA256 match before anything is written to disk.
"""
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import updates


BIN_NAME = 'XREAL-one-pro-Pilot2-RU-0.7.0-perf-d6653cf-UI90-HDR70.bin'
BIN_BYTES = b'\x00firmware-bytes-for-testing\xff' * 10
BIN_SHA = hashlib.sha256(BIN_BYTES).hexdigest()
BASE = f'https://github.com/{updates.OWNER}/{updates.REPO}/releases/download/perf-v24'
BIN_URL = f'{BASE}/{BIN_NAME}'
SUMS_URL = f'{BASE}/SHA256SUMS.txt'

RELEASES = [{
    'tag_name': 'perf-v24',
    'name': 'One Pro perf candidate',
    'prerelease': True,
    'published_at': '2026-11-13T00:00:00Z',
    'assets': [
        {'name': BIN_NAME, 'size': len(BIN_BYTES), 'browser_download_url': BIN_URL},
        {'name': 'SHA256SUMS.txt', 'size': 90, 'browser_download_url': SUMS_URL},
        {'name': 'build-report.json', 'size': 20,
         'browser_download_url': f'{BASE}/build-report.json'},
    ],
}]


def opener_for(mapping):
    def opener(url):
        if url not in mapping:
            raise AssertionError('unexpected URL requested: ' + url)
        return io.BytesIO(mapping[url])
    return opener


def default_map():
    return {
        updates.RELEASES_URL: json.dumps(RELEASES).encode(),
        SUMS_URL: f'{BIN_SHA}  {BIN_NAME}\n'.encode(),
        BIN_URL: BIN_BYTES,
    }


class ListReleasesTests(unittest.TestCase):
    def test_lists_only_bin_assets_with_sha(self):
        releases = updates.list_releases(opener=opener_for(default_map()))
        self.assertEqual(len(releases), 1)
        release = releases[0]
        self.assertEqual(release.tag, 'perf-v24')
        self.assertTrue(release.prerelease)
        self.assertEqual(len(release.bins), 1)  # the .json and .txt are excluded
        asset = release.bins[0]
        self.assertEqual(asset.name, BIN_NAME)
        self.assertEqual(asset.url, BIN_URL)
        self.assertEqual(asset.sha256, BIN_SHA)

    def test_release_without_bins_is_dropped(self):
        data = {updates.RELEASES_URL: json.dumps([
            {'tag_name': 'docs', 'assets': [
                {'name': 'notes.txt', 'browser_download_url': f'{BASE}/notes.txt'}]}
        ]).encode()}
        self.assertEqual(updates.list_releases(opener=opener_for(data)), [])

    def test_asset_on_foreign_host_is_skipped(self):
        evil = f'https://evil.example.com/{BIN_NAME}'
        data = {updates.RELEASES_URL: json.dumps([
            {'tag_name': 't', 'assets': [
                {'name': BIN_NAME, 'size': 1, 'browser_download_url': evil}]}
        ]).encode()}
        self.assertEqual(updates.list_releases(opener=opener_for(data)), [])


class DownloadTests(unittest.TestCase):
    def test_downloads_and_verifies(self):
        asset = updates.Asset(BIN_NAME, BIN_URL, len(BIN_BYTES), BIN_SHA)
        with tempfile.TemporaryDirectory() as folder:
            seen = []
            path = updates.download(asset, folder, opener=opener_for(default_map()),
                                    progress=lambda got, total: seen.append(got))
            self.assertEqual(Path(path).read_bytes(), BIN_BYTES)
            self.assertEqual(Path(path).name, BIN_NAME)
            self.assertTrue(seen and seen[-1] == len(BIN_BYTES))

    def test_sha_mismatch_is_not_written(self):
        asset = updates.Asset(BIN_NAME, BIN_URL, len(BIN_BYTES), 'a' * 64)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(RuntimeError):
                updates.download(asset, folder, opener=opener_for(default_map()))
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_oversize_download_is_rejected(self):
        asset = updates.Asset(BIN_NAME, BIN_URL, len(BIN_BYTES), BIN_SHA)
        original = updates.MAX_BIN
        updates.MAX_BIN = 8
        try:
            with tempfile.TemporaryDirectory() as folder:
                with self.assertRaises(RuntimeError):
                    updates.download(asset, folder, opener=opener_for(default_map()))
                self.assertEqual(list(Path(folder).iterdir()), [])
        finally:
            updates.MAX_BIN = original

    def test_foreign_host_download_refused_before_opener(self):
        asset = updates.Asset(BIN_NAME, 'https://evil.example.com/x.bin', 1, None)
        def opener(url):
            raise AssertionError('opener must not be called for a foreign host')
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(RuntimeError):
                updates.download(asset, folder, opener=opener)

    def test_plain_http_is_refused(self):
        with self.assertRaises(RuntimeError):
            updates._check_url('http://github.com/davnozdu/x/releases/download/t/a.bin')


class CheckUrlTests(unittest.TestCase):
    def test_allowed_hosts_pass(self):
        updates._check_url(updates.RELEASES_URL)
        updates._check_url(BIN_URL)
        updates._check_url('https://objects.githubusercontent.com/github-production/x')

    def test_foreign_host_fails(self):
        for bad in ('https://example.com/a.bin', 'https://raw.githubusercontent.com/a',
                    'ftp://github.com/a'):
            with self.assertRaises(RuntimeError):
                updates._check_url(bad)


if __name__ == '__main__':
    unittest.main()
