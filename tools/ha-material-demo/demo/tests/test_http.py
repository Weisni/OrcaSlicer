import json
import tempfile
import threading
import unittest
import urllib.request
import uuid
from pathlib import Path
from urllib.error import HTTPError
from server import make_server
from custom_components.quack_material_demo.native_bridge import TABLES


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.server = make_server(Path(self.temp.name) / 'demo.sqlite3', port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def test_snapshot_and_assignment_use_same_contract_as_ha(self):
        data = json.load(urllib.request.urlopen(self.url + '/api/quack_material_demo/materials'))
        request = urllib.request.Request(self.url + '/api/quack_material_demo/action/assign',
            json.dumps({'slot': 'A1', 'spool_uuid': data['spools'][0]['uuid'], 'revision': 0}).encode(),
            {'Content-Type': 'application/json'})
        self.assertFalse(json.load(urllib.request.urlopen(request))['device_command_sent'])

    def test_second_fixture_server_cannot_share_the_listening_port(self):
        duplicate=None
        try:
            with self.assertRaises(OSError):
                duplicate=make_server(Path(self.temp.name)/'duplicate.sqlite3',port=self.server.server_port)
        finally:
            if duplicate is not None:duplicate.server_close()

    def test_cross_origin_mutation_is_rejected(self):
        request = urllib.request.Request(self.url + '/api/quack_material_demo/action/assign', b'{}',
            {'Content-Type': 'application/json', 'Origin': 'https://untrusted.example'})
        with self.assertRaises(HTTPError) as caught:
            urllib.request.urlopen(request)
        self.assertEqual(caught.exception.code, 403)
        caught.exception.close()

    def test_malformed_body_returns_client_error(self):
        request = urllib.request.Request(self.url + '/api/quack_material_demo/action/start', b'[]',
            {'Content-Type': 'application/json'})
        with self.assertRaises(HTTPError) as caught:
            urllib.request.urlopen(request)
        self.assertEqual(caught.exception.code, 400)
        caught.exception.close()

    def test_fixture_supports_lite_pages_delta_profiles_and_recovery(self):
        def get(path):
            return json.load(urllib.request.urlopen(self.url+'/api/quack_material_demo/'+path))
        def post(action,payload):
            request=urllib.request.Request(self.url+'/api/quack_material_demo/action/'+action,
                json.dumps(payload).encode(),{'Content-Type':'application/json'})
            return json.load(urllib.request.urlopen(request))
        post('native_sync',dict(revision=0,bundle=dict(schema_version=8,tables={t:[] for t in TABLES})))
        lite=get('materials?view=provider')
        self.assertNotIn('native_bundle',lite)
        page=get('native_snapshot?table=spools&limit=1&revision='+str(lite['revision']))
        self.assertEqual(len(page['rows']),1)
        old=page['rows'][0];new=dict(old,name='Changed through delta')
        ack=post('provider_delta',dict(request_key='http-delta',revision=lite['revision'],
            changes=[dict(table='spools',before=old,after=new)]))
        self.assertTrue(ack['accepted'])
        with self.assertRaises(HTTPError) as caught:get('native_snapshot?table=spools&revision='+str(lite['revision']))
        self.assertEqual(caught.exception.code,409);caught.exception.close()
        from test_profiles import profile
        payload=profile()
        post('native_apply',dict(request_key='http-profile',revision=ack['revision'],confirmed=True,response='ack',
            changes=[dict(spool_uuid=old['id'],fields=dict(filament_preset_id=payload['name']),
                expected=dict(filament_preset_id=old['filament_preset_id']),
                material_profile=payload,expected_profile_sha256=None)]))
        full=get('profile?spool_uuid='+old['id']+'&sha256='+payload['sha256'])
        self.assertEqual(full,payload)
        exported=get('recovery')
        self.assertEqual(exported['format'],'quack-ha-recovery')
        self.assertIn('material_profiles',exported['tables'])

    def test_provider_graph_pair_above_one_megabyte_matches_ha_body_limit(self):
        self.server.shutdown();self.server.server_close();self.thread.join()
        self.server=make_server(Path(self.temp.name)/'provider.sqlite3',port=0,settings={'mode':'pilot'})
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.url=f'http://127.0.0.1:{self.server.server_port}'
        def post(payload):
            body=json.dumps(payload).encode()
            request=urllib.request.Request(self.url+'/api/quack_material_demo/action/provider_apply',
                body,{'Content-Type':'application/json'})
            return json.load(urllib.request.urlopen(request))
        empty={'schema_version':8,'tables':{table:[] for table in TABLES}}
        bundle=json.loads(json.dumps(empty))
        bundle['tables']['customers']=[dict(id=str(uuid.uuid4()),name='Customer '+str(i),notes='x'*8000) for i in range(70)]
        snapshot=post(dict(request_key='large-seed',revision=0,before=empty,bundle=bundle))
        updated=json.loads(json.dumps(snapshot['native_bundle']))
        updated['tables']['customers'][0]['name']='Updated customer'
        request=dict(request_key='large-edit',revision=snapshot['revision'],before=snapshot['native_bundle'],bundle=updated)
        self.assertGreater(len(json.dumps(request).encode()),1000000)
        result=post(request)
        self.assertEqual(result['native_bundle']['tables']['customers'][0]['name'],'Updated customer')
