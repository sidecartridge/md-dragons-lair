// A worker of the page: the clip files into DLAIR.zip, for a browser that
// cannot write to a folder the user picks. The zip is written on disk, in
// the browser's private storage for this site (the origin private file
// system), never held in memory: a set is about 540 MB. Stored, not
// compressed (the clips do not compress), one entry per file.
//
// Messages in:  {type: 'open'}, {type: 'add', name, bytes}, {type: 'close'}
// Messages out: {type: 'opened'} or {type: 'error', message},
//               {type: 'added', name}, {type: 'closed', file}

const ZIP_NAME = 'DLAIR.zip';

let handle = null;
let pos = 0;
let central = [];  // the central directory's entries

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function dosTime(d) {
  return (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
}

function dosDate(d) {
  return ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
}

function put(bytes) {
  handle.write(bytes, { at: pos });
  pos += bytes.length;
}

function add(name, bytes) {
  const nameBytes = new TextEncoder().encode(name);
  const crc = crc32(bytes);
  const now = new Date();
  const local = new DataView(new ArrayBuffer(30));
  local.setUint32(0, 0x04034b50, true);
  local.setUint16(4, 20, true);  // version needed: 2.0
  local.setUint16(8, 0, true);   // stored
  local.setUint16(10, dosTime(now), true);
  local.setUint16(12, dosDate(now), true);
  local.setUint32(14, crc, true);
  local.setUint32(18, bytes.length, true);
  local.setUint32(22, bytes.length, true);
  local.setUint16(26, nameBytes.length, true);
  const offset = pos;
  put(new Uint8Array(local.buffer));
  put(nameBytes);
  put(bytes);
  central.push({ nameBytes, crc, size: bytes.length, offset, now });
}

function close() {
  const start = pos;
  for (const e of central) {
    const d = new DataView(new ArrayBuffer(46));
    d.setUint32(0, 0x02014b50, true);
    d.setUint16(4, 20, true);
    d.setUint16(6, 20, true);
    d.setUint16(12, dosTime(e.now), true);
    d.setUint16(14, dosDate(e.now), true);
    d.setUint32(16, e.crc, true);
    d.setUint32(20, e.size, true);
    d.setUint32(24, e.size, true);
    d.setUint16(28, e.nameBytes.length, true);
    d.setUint32(42, e.offset, true);
    put(new Uint8Array(d.buffer));
    put(e.nameBytes);
  }
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, central.length, true);
  end.setUint16(10, central.length, true);
  end.setUint32(12, pos - start, true);
  end.setUint32(16, start, true);
  put(new Uint8Array(end.buffer));
  handle.flush();
  handle.close();
  handle = null;
}

onmessage = async (event) => {
  const msg = event.data;
  try {
    if (msg.type === 'open') {
      const root = await navigator.storage.getDirectory();
      const file = await root.getFileHandle(ZIP_NAME, { create: true });
      handle = await file.createSyncAccessHandle();
      handle.truncate(0);
      pos = 0;
      central = [];
      postMessage({ type: 'opened' });
    } else if (msg.type === 'add') {
      add(msg.name, new Uint8Array(msg.bytes));
      postMessage({ type: 'added', name: msg.name });
    } else if (msg.type === 'close') {
      close();
      const root = await navigator.storage.getDirectory();
      const file = await (await root.getFileHandle(ZIP_NAME)).getFile();
      postMessage({ type: 'closed', file });
    }
  } catch (err) {
    postMessage({ type: 'error', message: String(err && err.message ? err.message : err) });
  }
};
