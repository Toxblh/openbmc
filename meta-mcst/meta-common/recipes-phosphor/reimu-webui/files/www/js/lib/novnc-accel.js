// Faster JPEG path for noVNC on top of obmc-ikvm.
//
// obmc-ikvm forwards frames compressed by the AST2500 video engine as-is
// (Tight/JPEG rectangles), so the BMC never re-encodes video. Stock noVNC
// then turns each frame into a base64 data: URL and decodes it with an <img>,
// which costs a base64 pass over ~100-300 KiB per frame on the main thread.
// Here frames are decoded with createImageBitmap(), which browsers run off
// the main thread and, where available, on the GPU/hardware JPEG decoder.
// When the decoder falls behind, full-screen frames that are already
// superseded by a newer full-screen frame are dropped instead of queued,
// which keeps latency bounded on slow clients.

import { Display } from "../../vendor/novnc/novnc.js";

export const accelStats = { frames: 0, dropped: 0, decodeMs: 0, enabled: false };

export function installAccel() {
  if (Display.prototype._reimuAccel || typeof createImageBitmap !== "function") return accelStats.enabled;
  Display.prototype._reimuAccel = true;
  accelStats.enabled = true;

  const origImageRect = Display.prototype.imageRect;
  Display.prototype.imageRect = function (x, y, width, height, mime, arr) {
    if (width === 0 || height === 0) return;
    if (mime !== "image/jpeg") return origImageRect.call(this, x, y, width, height, mime, arr);

    const full = x === 0 && y === 0 && width === this._fbWidth && height === this._fbHeight;
    if (full) {
      // queue[0] may be the entry being waited on right now; never touch it
      for (let i = this._renderQ.length - 1; i >= 1; i--) {
        const a = this._renderQ[i];
        if (a.type === "frame" && a.frame._reimuFull) {
          this._renderQ.splice(i, 1);
          a.frame._dropped = true;
          if (a.frame.frame) a.frame.frame.close();
          accelStats.dropped++;
        }
      }
    }

    const entry = { ready: false, frame: null, promise: null, _reimuFull: full, _dropped: false };
    const t0 = performance.now();
    // arr may be a view into a reused receive buffer: copy before async use
    const blob = new Blob([arr.slice()], { type: mime });
    entry.promise = createImageBitmap(blob).then(
      (bmp) => {
        accelStats.decodeMs = accelStats.decodeMs * 0.9 + (performance.now() - t0) * 0.1;
        if (entry._dropped) {
          bmp.close();
          return;
        }
        entry.frame = bmp;
        entry.ready = true;
        accelStats.frames++;
      },
      () => {
        // undecodable frame: render nothing but keep the queue moving
        entry.frame = { close() {}, width: 0, height: 0 };
        entry.ready = true;
      },
    );
    this._renderQPush({ type: "frame", frame: entry, x, y, width, height });
  };

  // noVNC's 'frame' renderer draws with the 9-argument drawImage; for an
  // ImageBitmap with zero size (failed decode) skip drawing.
  const origDrawImage = Display.prototype.drawImage;
  Display.prototype.drawImage = function (img, ...args) {
    if (img && img.width === 0 && img.height === 0 && !(img instanceof HTMLImageElement)) return;
    return origDrawImage.call(this, img, ...args);
  };
  return true;
}
