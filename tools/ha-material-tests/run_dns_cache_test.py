"""Exercise resolver reuse with two real HTTP transfers to a local server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import threading
import sys
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.end_headers(); self.wfile.write(b'ok')
    def log_message(self, *args): pass
server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
try:
    result = subprocess.run([sys.argv[1], str(server.server_port)], timeout=15)
finally:
    server.shutdown(); server.server_close(); thread.join()
raise SystemExit(result.returncode)
