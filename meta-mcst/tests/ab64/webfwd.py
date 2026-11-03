"""The web port of a BMC in QEMU, as the browser sees it on the board.

QEMU's user network (slirp) takes the connections of its hostfwd slowly: a few
at once (a page loading its scripts) and some are reset before they reach the
guest (none counted in its TCP PassiveOpens), more so on a loaded machine; the
page then has no app.js and no login form. The board has no slirp. So the tests
listen on the port themselves (a real backlog) and pass each connection on to
QEMU's: what the client sent is kept until the BMC answers, and a connection
reset before that is opened again and given it again (only a TLS ClientHello
goes first: nothing done twice).

    inner(PORT) -> the port to give QEMU's hostfwd instead of PORT
"""
import select, socket, threading, time

TRIES = 8
_inner = {}
_lock = threading.Lock()


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def _pipe(a, b):
    """both ways until one side closes"""
    socks = [a, b]
    while True:
        r, _, _ = select.select(socks, [], [], 300)
        if not r:
            continue
        for s in r:
            try:
                d = s.recv(65536)
            except OSError:
                d = b""
            if not d:
                return
            try:
                (b if s is a else a).sendall(d)
            except OSError:
                return


def _serve(client, port):
    sent, up = b"", None
    try:
        for i in range(TRIES):
            try:
                up = socket.create_connection(("127.0.0.1", port), timeout=30)
                up.settimeout(None)
                if sent:
                    up.sendall(sent)
                # until the BMC's first byte: the client's bytes kept
                while True:
                    r, _, _ = select.select([client, up], [], [], 120)
                    if not r:
                        raise OSError("no answer")
                    if client in r:
                        d = client.recv(65536)
                        if not d:
                            return
                        sent += d
                        up.sendall(d)
                    if up in r:
                        d = up.recv(65536)
                        if not d:
                            raise OSError("closed before an answer")
                        client.sendall(d)
                        _pipe(client, up)
                        return
            except OSError:
                if up:
                    up.close()
                    up = None
                time.sleep(0.2 * (i + 1))
    finally:
        for s in (client, up):
            if s:
                try:
                    s.close()
                except OSError:
                    pass


def _listen(srv, port):
    while True:
        c, _ = srv.accept()
        threading.Thread(target=_serve, args=(c, port), daemon=True).start()


def inner(outer):
    with _lock:
        if outer not in _inner:
            srv = socket.socket()
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(("127.0.0.1", outer))
            srv.listen(512)
            _inner[outer] = _free_port()
            threading.Thread(target=_listen, args=(srv, _inner[outer]), daemon=True).start()
        return _inner[outer]
