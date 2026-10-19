// Worker owning a FileSystemSyncAccessHandle for an image stored in the
// Origin Private File System (only workers may use synchronous handles).
let handle = null;

self.onmessage = async (ev) => {
  const { id, op } = ev.data;
  try {
    let result;
    switch (op) {
      case "open": {
        const root = await navigator.storage.getDirectory();
        const dir = await root.getDirectoryHandle("images", { create: true });
        const fh = await dir.getFileHandle(ev.data.name);
        handle = await fh.createSyncAccessHandle();
        result = handle.getSize();
        break;
      }
      case "read": {
        const buf = new Uint8Array(ev.data.len);
        const n = handle.read(buf, { at: ev.data.off });
        if (n < buf.length) buf.fill(0, n);
        self.postMessage({ id, ok: true, result: buf.buffer }, [buf.buffer]);
        return;
      }
      case "write": {
        const data = new Uint8Array(ev.data.data);
        const n = handle.write(data, { at: ev.data.off });
        if (n !== data.length) throw new Error(`short write ${n}/${data.length}`);
        break;
      }
      case "flush":
        handle.flush();
        break;
      case "close":
        if (handle) {
          handle.flush();
          handle.close();
          handle = null;
        }
        break;
      default:
        throw new Error(`unknown op ${op}`);
    }
    self.postMessage({ id, ok: true, result });
  } catch (e) {
    self.postMessage({ id, ok: false, error: String(e && e.message ? e.message : e) });
  }
};
