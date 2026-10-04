import unittest
from custom_components.quack_material_demo.access import may_read,may_write
class AccessTests(unittest.TestCase):
    def test_permissions_are_inventory_scoped(self):
        from types import SimpleNamespace
        admin=SimpleNamespace(is_admin=True,id='a');sync=SimpleNamespace(is_admin=False,id='q');other=SimpleNamespace(is_admin=False,id='b')
        self.assertTrue(may_read(admin,[]));self.assertTrue(may_read(sync,['q']));self.assertFalse(may_read(other,['q']))
        self.assertTrue(may_write(admin,[],'customer'));self.assertTrue(may_write(sync,['q'],'native_apply'))
        self.assertFalse(may_write(sync,['q'],'native_sync'));self.assertTrue(may_write(admin,[],'native_sync'))
        self.assertFalse(may_write(sync,['q'],'customer'));self.assertFalse(may_write(other,['q'],'native_sync'))
        self.assertTrue(may_write(sync,['q'],'provider_apply'));self.assertTrue(may_write(sync,['q'],'provider_job'))
        self.assertFalse(may_write(other,['q'],'provider_apply'));self.assertFalse(may_write(other,['q'],'provider_job'))
