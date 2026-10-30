"""Validate the exact release matrix and write a SHA-256 manifest before upload."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def expected_assets(version, self_hosted):
    names = {
        f'QuackSlicer_Mac_universal_V{version}.dmg',
        f'QuackSlicer_Linux_AppImage_Ubuntu2404_V{version}.AppImage',
        f'QuackSlicer_Windows_V{version}_x64_portable.zip',
    }
    if not self_hosted:
        names.update({
            f'QuackSlicer_Linux_AppImage_Ubuntu2404_aarch64_V{version}.AppImage',
            f'Debug_PDB_V{version}_for_developers_only.7z',
        })
        for arch in ('x64', 'arm64'):
            names.add(f'QuackSlicer_Windows_Installer_V{version}_{arch}.exe')
        names.add(f'QuackSlicer_Windows_V{version}_arm64_portable.zip')
        for arch in ('x86_64', 'aarch64'):
            names.add(f'QuackSlicer-Linux-flatpak_V{version}_{arch}.flatpak')
    return names


def validate(directory, tag, version, self_hosted):
    if self_hosted:
        raise ValueError('Self-hosted builds have an incomplete supported-platform release matrix')
    if '.local' in version:
        raise ValueError('A local test version cannot be released')
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?', version) or tag != 'v' + version:
        raise ValueError('Release tag and source version must match exactly')
    expected = expected_assets(version, self_hosted)
    checksums = {name + '.sha256' for name in expected if name.endswith('.exe')}
    actual = {p.name for p in directory.iterdir() if p.is_file()} - {'SHA256SUMS'}
    missing = (expected | checksums) - actual
    if missing:
        raise ValueError('Missing release assets: ' + ', '.join(sorted(missing)))
    unexpected = actual - expected - checksums
    if unexpected:
        raise ValueError('Unexpected release assets: ' + ', '.join(sorted(unexpected)))
    rows = []
    for name in sorted(actual):
        asset = directory / name
        if asset.stat().st_size == 0:
            raise ValueError('Empty release asset: ' + name)
        with asset.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if name in expected and name.endswith('.exe'):
            recorded = (directory / (name + '.sha256')).read_text().split()[0].lower()
            if digest != recorded:
                raise ValueError('Checksum mismatch: ' + name)
        rows.append(f'{digest}  {name}\n')
    (directory / 'SHA256SUMS').write_text(''.join(rows), encoding='utf-8')
    # Read every assembled asset again against the final manifest before upload.
    for row in rows:
        digest, name = row.rstrip('\n').split('  ', 1)
        with (directory / name).open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != digest:
                raise ValueError('Checksum changed during validation: ' + name)


def validate_remote(directory, release):
    local = {p.name: p for p in directory.iterdir() if p.is_file()}
    remote = {asset['name']: asset for asset in release['assets']}
    if local.keys() != remote.keys():
        raise ValueError('Uploaded release asset set differs from the verified matrix')
    for name, path in local.items():
        with path.open('rb') as stream:
            digest = 'sha256:' + hashlib.file_digest(stream, 'sha256').hexdigest()
        if remote[name].get('digest') != digest or remote[name].get('size') != path.stat().st_size:
            raise ValueError('Uploaded checksum or size mismatch: ' + name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--version-file', type=Path, default=Path('version.inc'))
    parser.add_argument('--matrix-file', type=Path, required=True)
    parser.add_argument('--head', required=True)
    parser.add_argument('--remote-json', type=Path)
    args = parser.parse_args()
    match = re.search(r'set\(SoftFever_VERSION "([^"]+)"\)', args.version_file.read_text())
    if not match:
        parser.error('Cannot read source version')
    try:
        matrix = json.loads(args.matrix_file.read_text())
        if matrix.get('head_sha') != args.head or type(matrix.get('self_hosted')) is not bool:
            raise ValueError('Source matrix does not match the reviewed build SHA')
        validate(args.directory, args.tag, match[1], matrix['self_hosted'])
        if args.remote_json:
            validate_remote(args.directory, json.loads(args.remote_json.read_text()))
    except ValueError as error:
        parser.exit(1, f'{error}\n')
