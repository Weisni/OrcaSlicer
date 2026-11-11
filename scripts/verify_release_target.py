"""Check a fresh draft and the peeled immutable tag before release mutations."""
import argparse
import json
import subprocess
from urllib.parse import quote


class MissingResource(RuntimeError):
    pass


def gh_api(endpoint, fields=None):
    command = ['gh', 'api', endpoint]
    if fields:
        command.extend(['--method', 'POST'])
        for key, value in fields.items():
            command.extend(['-f', f'{key}={value}'])
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        try:
            missing = str(json.loads(result.stdout).get('status')) == '404'
        except (ValueError, AttributeError):
            missing = False
        if missing:
            raise MissingResource(endpoint)
        raise RuntimeError(f'GitHub request failed: {endpoint}: {result.stderr.strip()}')
    return json.loads(result.stdout)


def verify(api, repository, tag, head, release_id, create_tag=False, require_tag=False):
    prefix = f'repos/{repository}'
    release = api(f'{prefix}/releases/{release_id}')
    if release.get('id') != release_id:
        raise ValueError('Release ID changed')
    if release.get('draft') is not True:
        raise ValueError('Release is no longer a draft; refusing mutation')
    if release.get('tag_name') != tag:
        raise ValueError('Release tag changed')
    if release.get('prerelease') is not ('-' in tag):
        raise ValueError('Release prerelease status must match the tag')
    target = api(f'{prefix}/commits/{quote(release["target_commitish"], safe="")}')['sha']
    if target != head:
        raise ValueError('Draft target differs from the reviewed source SHA')
    try:
        obj = api(f'{prefix}/git/ref/tags/{quote(tag, safe="")}')['object']
    except MissingResource:
        if create_tag:
            # POST creates only an absent ref. Never patch, replace or force an
            # existing tag; a concurrent creation fails rather than moving it.
            api(f'{prefix}/git/refs', {'ref': f'refs/tags/{tag}', 'sha': head})
            return verify(api, repository, tag, head, release_id, require_tag=True)
        if require_tag:
            raise ValueError('Release tag is missing before publication')
        return
    visited = set()
    while obj.get('type') == 'tag':
        if obj['sha'] in visited or len(visited) >= 16:
            raise ValueError('Release tag has an invalid annotation chain')
        visited.add(obj['sha'])
        obj = api(f'{prefix}/git/tags/{obj["sha"]}')['object']
    if obj.get('type') != 'commit' or obj.get('sha') != head:
        raise ValueError('Existing release tag differs from the reviewed source SHA')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--head', required=True)
    parser.add_argument('--release-id', type=int, required=True)
    parser.add_argument('--create-tag', action='store_true')
    parser.add_argument('--require-tag', action='store_true')
    args = parser.parse_args()
    try:
        verify(gh_api, args.repo, args.tag, args.head, args.release_id, args.create_tag, args.require_tag)
    except (RuntimeError, ValueError, KeyError) as error:
        parser.exit(1, f'{error}\n')
