"""Manual HA preheat HTTP gates stay separate from Quack job capabilities."""
import unittest
from tests import test_preheat_api as api_fixture

JOB = api_fixture.JOB


class ManualPreheatHttpTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.fixture = api_fixture.PreheatHttpTests()
        await self.fixture.asyncSetUp()
        self.addCleanup(self.fixture.doCleanups)

    def body(self, command='start'):
        if command == 'cancel':
            return dict(job_uuid=JOB, request_key='cancel-1', instance_id='ha-user', version=1)
        return dict(job_uuid=JOB, request_key='start-1', instance_id='ha-user', printer_id='P2S-test',
                    parameters=dict(enabled=True, chamber_target=55, preheat_bed_target=110, print_bed_target=110))

    async def test_manual_only_runtime_keeps_job_endpoints_disabled_and_get_read_only(self):
        f = self.fixture
        views = await f.setup(False, True)
        self.assertEqual(len(f.runtimes), 1)
        self.assertTrue(f.runtimes[0].started)
        view = views['api:quack_materials:manual_preheat']
        self.assertTrue(view.requires_auth)
        code, result = await view.get(f.request())
        self.assertEqual((code, result), (200, {'schema_version': 1, 'enabled': True, 'preheat': None}))
        self.assertEqual(f.runtimes[0].manual_commands, [])
        self.assertEqual((await views['api:quack_materials:preheat'].get(f.request(), JOB))[0], 409)
        self.assertEqual((await view.get(f.request(user='other')))[0], 403)
        self.assertEqual((await view.get(f.request(query={'start': 'true'})))[0], 400)
        self.assertIn('/api/quack_material_demo/manual_preheat', view.extra_urls)

    async def test_start_and_cancel_require_admin_and_forward_only_reviewed_shape(self):
        f = self.fixture
        views = await f.setup(False, True)
        view = views['api:quack_materials:manual_preheat_command']
        for command in ('start', 'cancel'):
            body = self.body(command)
            self.assertEqual((await view.post(f.request(body), command))[0], 403)
            self.assertEqual((await view.post(f.request(body, admin=True), command))[0], 200)
        self.assertEqual(f.runtimes[0].manual_commands, [('start', self.body()), ('cancel', self.body('cancel'))])
        self.assertEqual((await view.post(f.request(self.body(), user=None), 'start'))[0], 403)

    async def test_manual_body_rejects_unknown_commands_extra_fields_queries_and_limits(self):
        f = self.fixture
        views = await f.setup(False, True)
        view = views['api:quack_materials:manual_preheat_command']
        for body in ([], {}, dict(self.body(), entity_id='number.foreign'), dict(self.body(), job_uuid='invalid'),
                     dict(self.body(), request_key='x' * 4096)):
            self.assertEqual((await view.post(f.request(body, admin=True), 'start'))[0], 400)
        self.assertEqual((await view.post(f.request(self.body(), admin=True, length=4097), 'start'))[0], 400)
        self.assertEqual((await view.post(f.request(self.body(), admin=True, query={'force': '1'}), 'start'))[0], 400)
        self.assertEqual((await view.post(f.request(self.body(), admin=True), 'extend'))[0], 400)
        self.assertEqual(f.runtimes[0].manual_commands, [])

    async def test_manual_disabled_stays_disabled_even_when_job_runtime_exists(self):
        f = self.fixture
        views = await f.setup(True, False)
        self.assertEqual((await views['api:quack_materials:manual_preheat'].get(f.request()))[0], 409)
        self.assertEqual((await views['api:quack_materials:manual_preheat_command'].post(
            f.request(self.body(), admin=True), 'start'))[0], 409)
        self.assertEqual(f.frontend_urls, [])

    async def test_frontend_is_registered_only_for_explicit_manual_opt_in(self):
        f = self.fixture
        await f.setup(False, True)
        self.assertEqual(len(f.frontend_urls), 1)
        self.assertTrue(f.frontend_urls[0].startswith('/quack-materials/manual_preheat.js'))
        paths = [row[0] for row in f.static_paths]
        self.assertIn('/quack-materials/manual_preheat.js', paths)
        self.assertIn('/quack-material-demo/manual_preheat.js', paths)

    async def test_manual_feature_rejects_non_boolean_before_runtime_creation(self):
        with self.assertRaisesRegex(ValueError, 'manual_preheat_enabled'):
            await self.fixture.setup(False, 'true')
        self.assertEqual(self.fixture.runtimes, [])


if __name__ == '__main__': unittest.main()
