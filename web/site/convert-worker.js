// A worker of the page: the WebAssembly converter (dlconv.mjs), the
// cartridge's own conversion, on the user's CD-ROM image read from their
// disk. One clip at a time; the clip file comes back whole.
//
// Messages in:  {type: 'open', file, list}   the image (and its clips, with list)
//               {type: 'convert', job, index, bits}
//               {type: 'header', entries, count, bits}   a set's manifest header
// Messages out: {type: 'opened', clips, version} or {type: 'opened', error}
//               {type: 'progress', job, permille}
//               {type: 'done', job, bytes, entry} or {type: 'failed', job, error}
//               {type: 'header', header}

import createModule from './dlconv.mjs';

let m = null;
let out = new Uint8Array(0);  // the clip file being written
let outLen = 0;

function write(pos, bytes) {
  const end = pos + bytes.length;
  if (end > out.length) {
    let size = Math.max(out.length, 1 << 22);
    while (size < end) size *= 2;
    const bigger = new Uint8Array(size);
    bigger.set(out.subarray(0, outLen));
    out = bigger;
  }
  out.set(bytes, pos);
  outLen = Math.max(outLen, end);
  return 0;
}

async function open(file, list) {
  const reader = new FileReaderSync();
  m = await createModule({
    dlSize: () => file.size,
    dlRead: (pos, len) =>
      new Uint8Array(reader.readAsArrayBuffer(file.slice(pos, pos + len))),
    dlWrite: write,
  });
  const r = m._dl_mount();
  if (r !== 0) {
    postMessage({ type: 'opened', error: r });
    return;
  }
  const clips = [];
  if (list) {
    const n = m._dl_clip_count();
    for (let i = 0; i < n; i++) {
      clips.push({
        name: m.UTF8ToString(m._dl_clip_name(i)),
        size: m._dl_clip_size(i) >>> 0,
      });
    }
  }
  postMessage({ type: 'opened', clips, version: m._dl_converter_version() });
}

function convert(job, index, bits) {
  out = new Uint8Array(0);
  outLen = 0;
  let r = m._dl_convert_start(index, bits);
  let last = performance.now();
  while (r >= 0 && (r = m._dl_convert_step()) === 1) {
    const now = performance.now();
    if (now - last > 200) {
      last = now;
      postMessage({ type: 'progress', job, permille: m._dl_convert_progress() });
    }
  }
  if (r !== 0) {
    postMessage({ type: 'failed', job, error: r });
    return;
  }
  const p = m._dl_manifest_entry(index);
  const entry = m.HEAPU8.slice(p, p + 32);
  const bytes = out.buffer.slice(0, outLen);
  out = new Uint8Array(0);
  postMessage({ type: 'done', job, bytes, entry }, [bytes]);
}

function header(entries, count, bits) {
  const buf = m._malloc(entries.length);
  m.HEAPU8.set(entries, buf);
  const h = m._dl_manifest_header(buf, count, bits);
  const bytes = m.HEAPU8.slice(h, h + 32);
  m._free(buf);
  postMessage({ type: 'header', header: bytes });
}

onmessage = async (event) => {
  const msg = event.data;
  if (msg.type === 'open') {
    await open(msg.file, msg.list);
  } else if (msg.type === 'convert') {
    convert(msg.job, msg.index, msg.bits);
  } else if (msg.type === 'header') {
    header(msg.entries, msg.count, msg.bits);
  }
};
