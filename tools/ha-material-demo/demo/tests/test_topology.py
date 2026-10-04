"""One paired printer may expose a safe subset of its six verified slot IDs."""
import json
import tempfile
import unittest
from pathlib import Path

from custom_components.quack_material_demo.store import Store, Conflict, SLOTS


class TopologyTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)/'slots.sqlite3'
        self.store=Store(self.path)
        self.roll=self.store.snapshot()['spools'][0]['uuid']

    def tearDown(self):self.temp.cleanup()

    def reopen(self,slots):return Store(self.path,seed_demo=False,settings={'enabled_slots':slots})

    def dump(self):
        with self.store.connection() as db:return list(db.iterdump())

    def test_default_six_and_enabled_subset_preserve_disabled_rows(self):
        self.assertEqual([s['id'] for s in self.store.snapshot()['slots']],list(SLOTS))
        reduced=self.reopen(['EXT','A1'])
        self.assertEqual(reduced.active_slots,('A1','EXT'))
        self.assertEqual([s['id'] for s in reduced.snapshot()['slots']],['A1','EXT'])
        with reduced.connection() as db:self.assertEqual(db.execute('SELECT COUNT(*) FROM slots').fetchone()[0],6)
        with self.assertRaises((ValueError,Conflict)):reduced.assign('A2',self.roll,0)
        reduced.assign('A1',self.roll,0)
        self.assertEqual(reduced.snapshot()['slots'][0]['spool_uuid'],self.roll)

    def test_invalid_or_occupied_deactivation_does_not_mutate_database(self):
        for slots in ([],['A1','A1'],['B1'],'A1',None):
            before=self.dump()
            with self.assertRaises(ValueError):self.reopen(slots)
            self.assertEqual(self.dump(),before)
        self.store.assign('A2',self.roll,0)
        before=self.dump()
        with self.assertRaises((ValueError,Conflict)):self.reopen(['A1','EXT'])
        self.assertEqual(self.dump(),before)

    def test_unsettled_disabled_slot_cannot_be_hidden_even_if_binding_was_cleared(self):
        self.store.assign('A2',self.roll,0)
        self.store.start_job('Reserved slot',[dict(slot='A2',weight_mg=1000)],'pending-slot')
        with self.store.connection() as db:db.execute("UPDATE slots SET spool_uuid=NULL WHERE id='A2'")
        before=self.dump()
        with self.assertRaises((ValueError,Conflict)):self.reopen(['A1','EXT'])
        self.assertEqual(self.dump(),before)

    def test_pending_assignment_and_uncertain_metadata_prevent_deactivation(self):
        with self.store.connection() as db:
            db.execute('CREATE TABLE printer_assignments(request_key TEXT PRIMARY KEY,request TEXT NOT NULL,operation TEXT NOT NULL)')
            db.execute('INSERT INTO printer_assignments VALUES (?,?,?)',('pending','{}',json.dumps(dict(slot='A2',status='pending'))))
        with self.assertRaises((ValueError,Conflict)):self.reopen(['A1','EXT'])
        with self.store.connection() as db:
            db.execute('UPDATE printer_assignments SET operation=?',(json.dumps(dict(slot='A2',status='cancelled')),))
            db.execute('CREATE TABLE printer_metadata_sync(slot TEXT PRIMARY KEY,operation TEXT NOT NULL)')
            db.execute('INSERT INTO printer_metadata_sync VALUES (?,?)',('A2',json.dumps(dict(slot='A2',status='uncertain'))))
        with self.assertRaises((ValueError,Conflict)):self.reopen(['A1','EXT'])
        with self.store.connection() as db:
            db.execute('UPDATE printer_metadata_sync SET operation=?',(json.dumps(dict(slot='A2',status='confirmed')),))
        self.assertEqual(len(self.reopen(['A1','EXT']).snapshot()['slots']),2)


if __name__=='__main__':unittest.main()
