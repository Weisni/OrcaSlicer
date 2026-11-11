"""Create a credential-free, offline snapshot for a Quack local test build."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import sqlite3
from pathlib import Path
from urllib.parse import urlsplit

SAFE_APP = set("language dark_color_mode curr_bed_type user_mode units use_inches show_axes show_labels show_plate_gridlines show_build_edges show_3d_navigator show_printable_box show_outline show_overhang use_perspective_camera auto_calculate_flush filaments_area_preferred_count group_filament_presets".split())
SAFE_ROOT = set("presets models filaments orca_presets nozzle_volume_types custom_color_list arrange".split())
SECRET_PARTS = ("password", "token", "secret", "access_code", "api_key", "print_host", "printhost", "host_web_ui", "ha_material_binding")


def scrub(value):
    if isinstance(value, dict):
        return {key: scrub(item) for key, item in value.items()
                if not any(part in key.lower() for part in SECRET_PARTS)}
    if isinstance(value, list):
        return [scrub(item) for item in value]
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variant", default="Beta")
    parser.add_argument("--version", default="2.5.24")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9]+", args.variant):
        parser.error("Variant must be alphanumeric")
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or source in output.parents or output in source.parents:
        parser.error("Source and output must be separate directory trees")
    output.mkdir(parents=True, exist_ok=False)
    raw = (source / "QuackSlicer.conf").read_text(encoding="utf-8-sig")
    original, _ = json.JSONDecoder().raw_decode(raw.lstrip())
    config = {key: scrub(value) for key, value in original.items() if key in SAFE_ROOT}
    config["app"] = {key: value for key, value in original.get("app", {}).items() if key in SAFE_APP}
    # Keep the server address so reconnecting does not silently target the local
    # demo port. Credentials and connection activation remain isolated.
    endpoint = original.get("app", {}).get("ha_material_demo_endpoint", "")
    try:
        address = urlsplit(endpoint)
        if (address.scheme in ("http", "https") and address.hostname and
                address.username is None and address.password is None and
                not address.query and not address.fragment):
            config["app"]["ha_material_demo_endpoint"] = endpoint
    except ValueError:
        pass
    config["app"].update({
        "ha_material_provider_enabled": "0", "ha_material_demo_enabled": "0",
        "passive_chamber_preheat": False, "sync_user_preset": False,
        "sync_system_preset": False, "preset_bundle_auto_update": False,
        "associate_3mf": False, "associate_stl": False, "associate_step": False,
        "associate_drc": False, "installed_networking": False,
        "stealth_mode": True, "show_home_page": False, "show_daily_tips": False,
        "show_hints": False, "show_splash_screen": False,
        "single_instance": False, "default_page": "0", "version": args.version,
    })
    config["firstguide"] = {"finish": "1"}
    config["header"] = f"Isolated local Quack {args.variant} test profile; production credentials excluded"
    body = json.dumps(config, ensure_ascii=False, indent="\t", sort_keys=True)
    checksum = hashlib.md5(body.encode()).hexdigest().upper()
    config_path = output / f"QuackSlicer{args.variant}Test.conf"
    config_path.write_text(body + "\n# MD5 checksum " + checksum + "\n", encoding="utf-8", newline="\n")
    # Merge saved profiles into the offline/default account, excluding account state.
    count = 0
    user = source / "user"
    active_account = original.get("app", {}).get("preset_folder", "") or "default"
    for account in sorted(user.iterdir(), key=lambda p: (p.name == active_account, p.name == "default", p.name)):
        if not account.is_dir():
            continue
        for kind in ("machine", "filament", "process"):
            for path in (account / kind).glob("*.json"):
                data = scrub(json.loads(path.read_text(encoding="utf-8-sig")))
                target = output / "user" / "default" / kind / path.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
                count += 1
    db = source / "filament_inventory" / "inventory.sqlite3"
    integrity = "not present"
    if db.exists():
        target = output / "filament_inventory" / db.name
        target.parent.mkdir(parents=True, exist_ok=True)
        # SQLite backup includes a consistent committed WAL snapshot without changing source.
        with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as src:
            with sqlite3.connect(target) as dst:
                src.backup(dst)
                integrity = dst.execute("PRAGMA integrity_check").fetchone()[0]
                if integrity != "ok":
                    raise RuntimeError("Copied inventory failed integrity check")
    manifest = {"profile_writes": count, "inventory_integrity": integrity,
                "credentials_copied": False, "networking_enabled": False,
                "source_config_sha256": hashlib.sha256(raw.encode()).hexdigest()}
    (output / "snapshot-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest))


if __name__ == "__main__":
    main()
