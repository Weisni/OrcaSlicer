import importlib.util
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('release_assets', Path(__file__).with_name('validate_release_assets.py'))
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)

class ReleaseAssetsTests(unittest.TestCase):
    def setUp(self):
        fixture_root = Path(__file__).resolve().parents[1] / 'build-test' / 'release-gate-fixtures'
        fixture_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=fixture_root)
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        for name in module.expected_assets('2.5.25-beta', False):
            (self.path / name).write_bytes(b'fixture payload')
        import hashlib
        for installer in self.path.glob('*.exe'):
            (self.path / (installer.name + '.sha256')).write_text(hashlib.sha256(installer.read_bytes()).hexdigest() + '  ' + installer.name)

    def test_complete_matrix_gets_verified_manifest(self):
        module.validate(self.path, 'v2.5.25-beta', '2.5.25-beta', False)
        self.assertIn('QuackSlicer-Linux-flatpak', (self.path / 'SHA256SUMS').read_text())

    def test_cli_rejects_matrix_from_other_commit(self):
        import json
        import subprocess
        import sys
        version_file = self.path.parent / (self.path.name + '-version.inc')
        version_file.write_text('set(SoftFever_VERSION "2.5.25-beta")')
        self.addCleanup(version_file.unlink)
        matrix_file = self.path.parent / (self.path.name + '-matrix.json')
        matrix_file.write_text(json.dumps({'head_sha': 'a' * 40, 'self_hosted': False}))
        self.addCleanup(matrix_file.unlink)
        command = [sys.executable, str(Path(__file__).with_name('validate_release_assets.py')),
                   str(self.path), '--tag', 'v2.5.25-beta', '--version-file', str(version_file),
                   '--matrix-file', str(matrix_file), '--head', 'b' * 40]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('reviewed build SHA', result.stderr)
        command[-1] = 'a' * 40
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_regular_release_matrix_is_accepted(self):
        for path in list(self.path.iterdir()):
            path.rename(path.with_name(path.name.replace('2.5.25-beta', '2.5.25')))
        module.validate(self.path, 'v2.5.25', '2.5.25', False)

    def test_missing_architecture_is_rejected(self):
        (self.path / 'QuackSlicer_Windows_V2.5.25-beta_arm64_portable.zip').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            module.validate(self.path, 'v2.5.25-beta', '2.5.25-beta', False)

    def test_corrupt_cpack_checksum_is_rejected(self):
        (self.path / 'QuackSlicer_Windows_Installer_V2.5.25-beta_x64.exe').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Checksum'):
            module.validate(self.path, 'v2.5.25-beta', '2.5.25-beta', False)

    def test_missing_installer_checksum_is_rejected(self):
        (self.path / 'QuackSlicer_Windows_Installer_V2.5.25-beta_arm64.exe.sha256').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            module.validate(self.path, 'v2.5.25-beta', '2.5.25-beta', False)

    def test_empty_payload_is_rejected(self):
        (self.path / 'QuackSlicer_Mac_universal_V2.5.25-beta.dmg').write_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'Empty'):
            module.validate(self.path, 'v2.5.25-beta', '2.5.25-beta', False)

    def test_self_hosted_matrix_cannot_qualify_full_release(self):
        expected = module.expected_assets('2.5.25-beta', True)
        for path in self.path.iterdir():
            if path.name not in expected:
                path.unlink()
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            module.validate(self.path, 'v2.5.25-beta', '2.5.25-beta', True)

    def test_uploaded_digest_and_unexpected_remote_assets_are_rejected(self):
        import hashlib
        release = {'assets': [{'name': p.name, 'size': p.stat().st_size,
                               'digest': 'sha256:' + hashlib.sha256(p.read_bytes()).hexdigest()}
                              for p in self.path.iterdir()]}
        module.validate_remote(self.path, release)
        release['assets'][0]['digest'] = 'sha256:' + '0' * 64
        with self.assertRaisesRegex(ValueError, 'Uploaded checksum'):
            module.validate_remote(self.path, release)
        release['assets'].append({'name': 'stale.exe'})
        with self.assertRaisesRegex(ValueError, 'asset set'):
            module.validate_remote(self.path, release)

    def test_wrong_version_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'version'):
            module.validate(self.path, 'v2.5.25', '2.5.25-beta', False)

    def test_local_version_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'local'):
            module.validate(self.path, 'v2.5.25-beta.local', '2.5.25-beta.local', False)

    def test_unexpected_asset_is_rejected(self):
        (self.path / 'quack-slicer.exe').write_bytes(b'not an installer')
        with self.assertRaisesRegex(ValueError, 'Unexpected'):
            module.validate(self.path, 'v2.5.25-beta', '2.5.25-beta', False)

if __name__ == '__main__':
    unittest.main()
