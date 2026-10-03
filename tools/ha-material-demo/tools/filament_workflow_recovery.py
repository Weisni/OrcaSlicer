"""Archive HA recovery JSON and closed Quack user-data; restore only to a NEW directory.

Account settings, access tokens, printer passwords and network configuration are
excluded by an allowlist. The archive still contains private inventory and profiles.
Project 3MF files must be backed up separately. Restored journals are not replayed.
"""
import argparse
import hashlib
import json
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'demo'))
from custom_components.quack_material_demo.recovery import restore_recovery, MAX_BYTES

JOURNALS={'ha-explicit-pending.json','ha-project-material-pending.json',
          'ha-authority-inventory-pending.json','ha-authority-job-pending.json'}
TOTAL_BYTES=192*1024*1024


def allowed(name):
    parts=PurePosixPath(name).parts
    if not parts or any(p in ('.','..') or ':' in p or '\\' in p for p in parts) or name.startswith('/'):
        return False
    if name=='ha-recovery.json':return True
    if len(parts)==2 and parts[0]=='quack' and parts[1] in JOURNALS:return True
    return (len(parts)>=5 and parts[:2]==('quack','user') and parts[3]=='filament'
            and parts[-1].endswith('.json'))


def digest(data):return hashlib.sha256(data).hexdigest()


def read_json_file(path):
    if path.stat().st_size>MAX_BYTES:raise ValueError('A recovery member exceeds 64 MiB')
    data=path.read_bytes()
    if len(data)>MAX_BYTES:raise ValueError('A recovery member exceeds 64 MiB')
    if not isinstance(json.loads(data),dict):raise ValueError('Recovery members must be JSON objects')
    return data


def create_bundle(ha_export,quack_data,output):
    """Call with Quack closed so a new pending request cannot escape the snapshot."""
    quack_data=Path(quack_data).resolve(strict=True);output=Path(output)
    if output.exists():raise FileExistsError('Archive already exists')
    files={'ha-recovery.json':read_json_file(Path(ha_export))}
    sources={}
    candidates=[quack_data/name for name in JOURNALS if (quack_data/name).is_file()]
    candidates+=list((quack_data/'user').glob('*/filament/**/*.json'))
    for path in sorted(candidates):
        if not path.resolve(strict=True).is_relative_to(quack_data):raise ValueError('Profile points outside Quack user-data')
        name='quack/'+path.relative_to(quack_data).as_posix()
        if not allowed(name):raise ValueError('Unsupported recovery member')
        files[name]=read_json_file(path);sources[path]=digest(files[name])
        if sum(map(len,files.values()))>TOTAL_BYTES:raise ValueError('Workflow recovery exceeds 192 MiB')
    # Verify the ledger before creating a supposedly recoverable archive.
    with tempfile.TemporaryDirectory() as temporary:
        restore_recovery(json.loads(files['ha-recovery.json']),Path(temporary)/'verify.sqlite3')
    if any(digest(read_json_file(path))!=value for path,value in sources.items()):
        raise ValueError('Quack files changed during export; close Quack and retry')
    manifest={'format':'quack-workflow-recovery','version':1,
              'files':{name:{'sha256':digest(data),'bytes':len(data)} for name,data in files.items()}}
    with zipfile.ZipFile(output,'x',compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('manifest.json',json.dumps(manifest,sort_keys=True))
        for name,data in files.items():archive.writestr(name,data)
    return {'files':len(files),'bytes':sum(map(len,files.values()))}


def restore_bundle(archive,destination):
    """Validate all hashes, paths and the ledger, without touching live HA or Quack."""
    destination=Path(destination)
    if destination.exists():raise FileExistsError('Restore destination already exists')
    with zipfile.ZipFile(archive) as source:
        info=source.infolist()
        if (len(info)>10000 or len({x.filename for x in info})!=len(info)
                or sum(x.file_size for x in info)>TOTAL_BYTES or any(x.file_size>MAX_BYTES for x in info)):
            raise ValueError('Invalid or oversized archive')
        if 'manifest.json' not in source.namelist():raise ValueError('Missing recovery manifest')
        manifest=json.loads(source.read('manifest.json'))
        if (not isinstance(manifest,dict) or manifest.get('format')!='quack-workflow-recovery'
                or type(manifest.get('version')) is not int or manifest['version']!=1
                or not isinstance(manifest.get('files'),dict)):
            raise ValueError('Unsupported recovery manifest')
        if set(source.namelist())!=set(manifest['files'])|{'manifest.json'} or 'ha-recovery.json' not in manifest['files']:
            raise ValueError('Unlisted or missing archive members')
        files={}
        for name,expected in manifest['files'].items():
            if not allowed(name) or not isinstance(expected,dict):raise ValueError('Invalid recovery path')
            data=source.read(name)
            if expected.get('bytes')!=len(data) or expected.get('sha256')!=digest(data):raise ValueError('Recovery member checksum mismatch')
            if not isinstance(json.loads(data),dict):raise ValueError('Recovery member is not a JSON object')
            files[name]=data
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        stage=Path(temporary)
        store=restore_recovery(json.loads(files['ha-recovery.json']),stage/'ha-inventory.sqlite3')
        snapshot=store.snapshot()
        for name,data in files.items():
            path=stage/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        # Exclusive creation refuses even a destination created during validation.
        destination.mkdir()
        for path in stage.iterdir():
            if path.is_dir():shutil.copytree(path,destination/path.name)
            else:shutil.copyfile(path,destination/path.name)
    return {'verified':True,'rolls':len(snapshot['spools']),'jobs':len(snapshot['jobs']),'files':len(files)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    create=sub.add_parser('create');create.add_argument('--ha',required=True,type=Path)
    create.add_argument('--quack-data',required=True,type=Path);create.add_argument('--output',required=True,type=Path)
    create.add_argument('--quack-closed',action='store_true',required=True,help='Confirm Quack is closed during capture')
    restore=sub.add_parser('restore');restore.add_argument('archive',type=Path);restore.add_argument('destination',type=Path)
    args=parser.parse_args()
    result=create_bundle(args.ha,args.quack_data,args.output) if args.command=='create' else restore_bundle(args.archive,args.destination)
    print(json.dumps(result))


if __name__=='__main__':main()
