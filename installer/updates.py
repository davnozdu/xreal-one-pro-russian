"""Download firmware BINs from this project's own GitHub releases, only.

Safety rules enforced here, not left to the caller:

* The owner/repo are hard-coded. The updater cannot be pointed elsewhere.
* Every request must be HTTPS to a GitHub host on ``ALLOWED_HOSTS``; redirects
  are re-checked against the same list, so a release asset cannot bounce the
  download to an arbitrary server.
* Requests carry only a generic ``User-Agent`` — never the user's name, email,
  machine or any other personal detail.
* A downloaded BIN is held in memory, its SHA256 compared against the release's
  ``SHA256SUMS.txt`` when present, and written to disk only on a match.

Standard library only; no third-party dependency is added by this module.
"""
from __future__ import annotations

import hashlib
import json
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

OWNER = 'davnozdu'
REPO = 'xreal-one-pro-russian'
RELEASES_URL = f'https://api.github.com/repos/{OWNER}/{REPO}/releases'
USER_AGENT = 'XREAL-Russian-Updater'
TIMEOUT = 30
MAX_BIN = 64 * 1024 * 1024
MAX_JSON = 4 * 1024 * 1024
MAX_SUMS = 1 * 1024 * 1024
ALLOWED_HOSTS = frozenset({
    'api.github.com', 'github.com', 'objects.githubusercontent.com',
    'release-assets.githubusercontent.com', 'codeload.github.com',
})


@dataclass(frozen=True)
class Asset:
    name: str
    url: str
    size: int
    sha256: str | None = None


@dataclass(frozen=True)
class Release:
    tag: str
    name: str
    prerelease: bool
    published_at: str
    bins: tuple


def _check_url(url):
    parts = urlsplit(url)
    if parts.scheme != 'https' or parts.hostname not in ALLOWED_HOSTS:
        raise RuntimeError(f'Недоверенный адрес загрузки: {url}')
    return url


class _SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        _check_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_BUILT = urllib.request.build_opener(_SafeRedirect)


def _default_opener(url):
    _check_url(url)
    request = urllib.request.Request(url, headers={
        'User-Agent': USER_AGENT, 'Accept': 'application/vnd.github+json'})
    return _BUILT.open(request, timeout=TIMEOUT)


def _read_capped(response, max_bytes, progress=None):
    try:
        total = int(response.headers.get('Content-Length'))
    except (TypeError, ValueError, AttributeError):
        total = None
    out = bytearray()
    while True:
        chunk = response.read(65536)
        if not chunk:
            break
        out += chunk
        if len(out) > max_bytes:
            raise RuntimeError('Загружаемый файл превышает допустимый размер.')
        if progress:
            progress(len(out), total)
    return bytes(out)


def _parse_sums(text):
    digests = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        if len(parts) >= 2 and len(parts[0]) == 64:
            name = parts[-1].lstrip('*').rsplit('/', 1)[-1]
            digests[name] = parts[0].lower()
    return digests


def list_releases(opener=None):
    """Return the releases of the pinned repo that carry at least one .bin."""
    opener = opener or _default_opener
    _check_url(RELEASES_URL)
    payload = _read_capped(opener(RELEASES_URL), MAX_JSON)
    releases = []
    for item in json.loads(payload.decode('utf-8')):
        assets = item.get('assets') or []
        sums = {}
        for asset in assets:
            name = (asset.get('name') or '')
            if name.upper().endswith('SHA256SUMS.TXT') or name.upper() == 'SHA256SUMS':
                url = asset.get('browser_download_url', '')
                try:
                    _check_url(url)
                    text = _read_capped(opener(url), MAX_SUMS).decode('utf-8', 'replace')
                    sums.update(_parse_sums(text))
                except Exception:
                    pass  # a missing/odd checksum file must not hide the release
        bins = []
        for asset in assets:
            name = asset.get('name') or ''
            if not name.lower().endswith('.bin'):
                continue
            url = asset.get('browser_download_url', '')
            try:
                _check_url(url)
            except RuntimeError:
                continue  # skip anything not served from GitHub
            bins.append(Asset(name=name, url=url, size=int(asset.get('size', 0)),
                              sha256=sums.get(name)))
        if bins:
            releases.append(Release(
                tag=item.get('tag_name', ''),
                name=item.get('name') or item.get('tag_name', ''),
                prerelease=bool(item.get('prerelease')),
                published_at=item.get('published_at', ''),
                bins=tuple(bins)))
    return releases


def download(asset, dest_dir, opener=None, progress=None):
    """Fetch one BIN asset, verify its SHA256, and write it into dest_dir."""
    opener = opener or _default_opener
    _check_url(asset.url)
    name = Path(asset.name).name
    if not name.lower().endswith('.bin'):
        raise RuntimeError('Ожидался файл прошивки .bin.')
    data = _read_capped(opener(asset.url), MAX_BIN, progress)
    digest = hashlib.sha256(data).hexdigest()
    if asset.sha256 and digest != asset.sha256.lower():
        raise RuntimeError('Контрольная сумма загруженного файла не совпала. '
                           'Файл не сохранён.')
    dest_dir = Path(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)
    path = dest_dir / name
    path.write_bytes(data)
    return path
