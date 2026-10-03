"""Verify a HA recovery export and restore it to a NEW isolated database.

Usage: python tools/restore_filament_recovery.py recovery.json new-inventory.sqlite3
This never replaces the running Home Assistant database.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'demo'))
from custom_components.quack_material_demo.recovery import restore_recovery

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('bundle',type=Path)
parser.add_argument('destination',type=Path)
args=parser.parse_args()
store=restore_recovery(json.loads(args.bundle.read_text(encoding='utf-8')),args.destination)
snapshot=store.snapshot()
print(json.dumps({'verified':True,'database':str(args.destination.resolve()),'rolls':len(snapshot['spools']),
    'jobs':len(snapshot['jobs']),'revision':snapshot['revision']}))
