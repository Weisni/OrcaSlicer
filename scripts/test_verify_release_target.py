import importlib.util
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('target', Path(__file__).with_name('verify_release_target.py'))
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)

class ReleaseTargetTests(unittest.TestCase):
    def setUp(self):
        self.head = 'a' * 40
        self.tag = 'v2.5.25-beta'
        self.target_sha = self.head
        self.release = {'draft': True, 'tag_name': self.tag, 'target_commitish': self.head}
        self.ref = {'object': {'type': 'commit', 'sha': self.head}}
        self.tag_objects = {}
        self.mutations = []

    def api(self, endpoint, fields=None):
        if endpoint.endswith('/releases/tags/' + self.tag):
            return self.release
        if '/commits/' in endpoint:
            return {'sha': self.target_sha}
        if endpoint.endswith('/git/ref/tags/' + self.tag):
            if self.ref is None:
                raise module.MissingResource(endpoint)
            return self.ref
        if '/git/tags/' in endpoint:
            return self.tag_objects[endpoint.rsplit('/', 1)[-1]]
        if endpoint.endswith('/git/refs') and fields:
            self.mutations.append(fields)
            self.ref = {'object': {'type': 'commit', 'sha': fields['sha']}}
            return self.ref
        self.fail(endpoint)

    def verify(self, **kwargs):
        return module.verify(self.api, 'Weisni/OrcaSlicer', self.tag, self.head, **kwargs)

    def test_matching_existing_tag_is_accepted_without_mutation(self):
        self.verify(require_tag=True)
        self.assertEqual(self.mutations, [])

    def test_regular_release_target_is_accepted(self):
        self.tag = 'v2.5.25'
        self.release['tag_name'] = self.tag
        self.verify(require_tag=True)

    def test_wrong_existing_tag_is_rejected_even_with_correct_target_commitish(self):
        self.ref['object']['sha'] = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'tag'):
            self.verify(create_tag=True)
        self.assertEqual(self.mutations, [])

    def test_annotated_tag_is_peeled(self):
        self.ref = {'object': {'type': 'tag', 'sha': 'c' * 40}}
        self.tag_objects['c' * 40] = {'object': {'type': 'commit', 'sha': self.head}}
        self.verify(require_tag=True)

    def test_wrong_annotated_tag_is_rejected(self):
        self.ref = {'object': {'type': 'tag', 'sha': 'c' * 40}}
        self.tag_objects['c' * 40] = {'object': {'type': 'commit', 'sha': 'b' * 40}}
        with self.assertRaisesRegex(ValueError, 'tag'):
            self.verify()

    def test_wrong_draft_target_is_rejected_before_creating_tag(self):
        self.ref = None
        self.target_sha = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'Draft target'):
            self.verify(create_tag=True)
        self.assertEqual(self.mutations, [])

    def test_missing_tag_is_created_only_at_the_reviewed_commit(self):
        self.ref = None
        self.verify(create_tag=True)
        self.assertEqual(self.mutations, [{'ref': 'refs/tags/' + self.tag, 'sha': self.head}])

    def test_published_release_cannot_be_mutated(self):
        self.release['draft'] = False
        self.ref = None
        with self.assertRaisesRegex(ValueError, 'draft'):
            self.verify(create_tag=True)
        self.assertEqual(self.mutations, [])

    def test_missing_tag_is_rejected_before_publish(self):
        self.ref = None
        with self.assertRaisesRegex(ValueError, 'missing'):
            self.verify(require_tag=True)

    def test_draft_is_checked_again_after_tag_creation(self):
        self.ref = None
        original_api = self.api
        def racing_api(endpoint, fields=None):
            result = original_api(endpoint, fields)
            if fields:
                self.release['draft'] = False
            return result
        with self.assertRaisesRegex(ValueError, 'draft'):
            module.verify(racing_api, 'Weisni/OrcaSlicer', self.tag, self.head, create_tag=True)

if __name__ == '__main__':
    unittest.main()
