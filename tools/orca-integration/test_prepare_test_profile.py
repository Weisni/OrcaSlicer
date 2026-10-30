"""Regression tests for the isolated test-profile handoff."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("prepare_test_profile.py")

class ProfileSnapshotTests(unittest.TestCase):
    def snapshot(self, endpoint):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "source", root / "test"
            (source / "user").mkdir(parents=True)
            original = json.dumps({"app": {
                "ha_material_demo_endpoint": endpoint,
                "ha_material_provider_enabled": "1",
                "ha_material_demo_enabled": "1",
                "ha_access_token": "MUST_NOT_BE_COPIED",
            }})
            config_path = source / "QuackSlicer.conf"
            config_path.write_text(original, encoding="utf-8")
            subprocess.run([sys.executable, str(SCRIPT), "--source", str(source),
                            "--output", str(output)], check=True, capture_output=True)
            text = (output / "QuackSlicerBetaTest.conf").read_text(encoding="utf-8")
            self.assertNotIn("MUST_NOT_BE_COPIED", text)
            self.assertEqual(config_path.read_text(encoding="utf-8"), original)
            data, _ = json.JSONDecoder().raw_decode(text)
            self.assertEqual(data["app"]["ha_material_provider_enabled"], "0")
            self.assertEqual(data["app"]["ha_material_demo_enabled"], "0")
            return data["app"]

    def test_valid_ha_address_survives_without_enabling_connection(self):
        endpoint = "http://homeassistant.local:8123/api/quack_material_demo/materials"
        self.assertEqual(self.snapshot(endpoint).get("ha_material_demo_endpoint"), endpoint)

    def test_endpoint_credentials_and_query_tokens_are_not_copied(self):
        for endpoint in (
            "https://user:secret@ha.example/api/quack_material_demo/materials",
            "https://ha.example/api/quack_material_demo/materials?token=secret",
            "https://ha.example/api/quack_material_demo/materials#secret",
        ):
            with self.subTest(endpoint=endpoint):
                self.assertNotIn("ha_material_demo_endpoint", self.snapshot(endpoint))

if __name__ == "__main__":
    unittest.main()
