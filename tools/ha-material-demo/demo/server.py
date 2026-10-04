"""Loopback-only development host for the same API used by HA."""
import argparse
import json
import socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
from custom_components.quack_material_demo.store import Store, Conflict
from custom_components.quack_material_demo.read_api import materials, native_page, profile
from custom_components.quack_material_demo.recovery import export_recovery
from custom_components.quack_material_demo.accounting import accounting

BASE = '/api/quack_material_demo/'
CARD = Path(__file__).parent / 'custom_components/quack_material_demo/card.js'


class FixtureHTTPServer(ThreadingHTTPServer):
    # Windows SO_REUSEADDR permits two live servers on the same port, leaving
    # requests routed to an obsolete handler after a development restart.
    allow_reuse_address = False

    def server_bind(self):
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def make_server(database, port=8765, settings=None):
    store = Store(database,seed_demo=(settings or {}).get('mode')!='pilot',settings=settings)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass  # Do not log payloads, URLs or credentials.

        def send(self, status, body, content_type='application/json'):
            data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
            self.send_response(status)
            self.send_header('Content-Type', content_type + '; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(data)

        def trusted_host(self):
            return self.headers.get('Host') in (f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}')

        def do_GET(self):
            if not self.trusted_host():
                return self.send(403, {'error': 'Loopback host required'})
            parsed=urlsplit(self.path)
            if parsed.path in (BASE+'materials',BASE+'native_snapshot',BASE+'profile',BASE+'recovery',BASE+'accounting'):
                try:
                    values=parse_qs(parsed.query,keep_blank_values=True)
                    if any(len(value)!=1 for value in values.values()):raise ValueError('Duplicate query parameter')
                    query={key:value[0] for key,value in values.items()}
                    if parsed.path==BASE+'recovery':
                        if query:raise ValueError('Invalid recovery query')
                        return self.send(200,export_recovery(store))
                    handler={BASE+'materials':materials,BASE+'native_snapshot':native_page,BASE+'profile':profile,BASE+'accounting':accounting}[parsed.path]
                    return self.send(200,handler(store,query))
                except Conflict as error:
                    return self.send(409,{'error':str(error)})
                except (ValueError,KeyError,TypeError):
                    return self.send(400,{'error':'Invalid inventory query'})
            if self.path == BASE + 'inventory':
                return self.send(200, dict(store.management_snapshot(),can_edit=True))
            if self.path == '/quack-material-demo/inventory.js':
                return self.send(200, CARD.with_name('inventory.js').read_text(encoding='utf-8'), 'text/javascript')
            if self.path.split('?')[0] in ('/inventory','/dashboard-filament/rolls'):
                return self.send(200, '<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Quack Inventory Pilot</title><style>body{background:#10151d;color:#eee;font:16px system-ui;margin:15px}</style><script type="module" src="/quack-material-demo/inventory.js"></script><quack-inventory-card></quack-inventory-card>', 'text/html')
            if self.path == '/quack-material-demo/card.js':
                return self.send(200, CARD.read_text(encoding='utf-8'), 'text/javascript')
            if self.path.startswith('/quack-material-demo/labels/'):
                name = self.path.rsplit('/', 1)[1]
                allowed = {'index.html'} | {s['uuid'] + '.svg' for s in store.snapshot()['spools']}
                if name not in allowed:
                    return self.send(404, {'error': 'Label not found'})
                label = CARD.parent / 'labels' / name
                if not label.is_file():
                    return self.send(404, {'error': 'Labels not generated'})
                return self.send(200, label.read_text(encoding='utf-8'), 'image/svg+xml' if name.endswith('.svg') else 'text/html')
            if self.path == '/':
                return self.send(200, '<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Quack Material Demo</title><style>body{background:#10151d;color:#eee;font:16px system-ui;margin:20px}quack-material-demo-card{display:block;max-width:1000px;margin:auto}</style><script type="module" src="/quack-material-demo/card.js"></script><quack-material-demo-card></quack-material-demo-card>', 'text/html')
            self.send(404, {'error': 'Not found'})

        def do_POST(self):
            origins = (f'http://127.0.0.1:{self.server.server_port}', f'http://localhost:{self.server.server_port}')
            if not self.trusted_host() or self.headers.get('Origin', origins[0]) not in origins:
                return self.send(403, {'error': 'Same-origin loopback requests only'})
            if not self.path.startswith(BASE + 'action/'):
                return self.send(404, {'error': 'Not found'})
            try:
                action = self.path.rsplit('/', 1)[1]
                limit = 2000000 if action in ('provider_apply','native_apply') else 1000000
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= limit or self.headers.get('Content-Type', '').split(';')[0] != 'application/json':
                    raise ValueError('Invalid JSON request')
                data = json.loads(self.rfile.read(length))
                return self.send(200, store.dispatch(action, data))
            except Conflict as error:
                self.send(409, {'error': str(error)})
            except (ValueError, KeyError, TypeError):
                self.send(400, {'error': 'Invalid demo request'})

    return FixtureHTTPServer(('127.0.0.1', port), Handler)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--database', default=str(Path(__file__).parent / 'runtime/demo.sqlite3'))
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--pilot', action='store_true')
    parser.add_argument('--settings', help='Private JSON settings file')
    args = parser.parse_args()
    settings=json.loads(Path(args.settings).read_text()) if args.settings else {}
    if args.pilot: settings['mode']='pilot'
    server = make_server(args.database, args.port, settings)
    print(f'Demo only; no device commands. Open http://127.0.0.1:{server.server_port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
