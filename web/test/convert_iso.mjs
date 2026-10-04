// Converts scene clips of a CD-ROM image with the WebAssembly converter in
// Node, as the web page will in a browser, and writes their clip files:
//
//   node web/test/convert_iso.mjs IMAGE BITS OUT_DIR [FIRST [COUNT]]
//
// BITS: 3 (an ST) or 4 (an STE). For each clip: its name, its file's size
// and CRC-32 (the header's), and the time it took. With every clip (no
// FIRST), the set's manifest too, SET.DLM.

import fs from 'node:fs';
import path from 'node:path';
import createModule from '../dist/dlconv.mjs';

const [image, bitsArg, outDir, firstArg, countArg] = process.argv.slice(2);
if (!image || !outDir || !(bitsArg === '3' || bitsArg === '4')) {
  console.error('usage: node convert_iso.mjs IMAGE 3|4 OUT_DIR [FIRST [COUNT]]');
  process.exit(2);
}
const bits = Number(bitsArg);
const fd = fs.openSync(image, 'r');
const size = fs.fstatSync(fd).size;
let out = -1;

const m = await createModule({
  dlSize: () => size,
  dlRead: (pos, len) => {
    const buf = Buffer.alloc(len);
    const got = fs.readSync(fd, buf, 0, len, pos);
    return buf.subarray(0, got);
  },
  dlWrite: (pos, bytes) => {
    fs.writeSync(out, bytes, 0, bytes.length, pos);
    return 0;
  },
});

const mounted = m._dl_mount();
if (mounted !== 0) {
  console.error(`${image}: not mounted (${mounted})`);
  process.exit(1);
}
const clips = m._dl_clip_count();
const first = Number(firstArg ?? 0);
const count = Math.min(Number(countArg ?? clips), clips - first);
fs.mkdirSync(outDir, { recursive: true });
let failed = 0;
const entries = [];
const t0 = performance.now();
for (let i = first; i < first + count; i++) {
  const name = m.UTF8ToString(m._dl_clip_name(i)).replace(/\.MPG$/, '');
  const file = path.join(outDir, `${name}.DLC`);
  out = fs.openSync(file, 'w');
  const t1 = performance.now();
  let r = m._dl_convert_start(i, bits);
  while (r >= 0 && (r = m._dl_convert_step()) === 1) {
    // a picture a call, as the page's worker will report progress
  }
  fs.closeSync(out);
  const crc = (m._dl_convert_crc() >>> 0).toString(16).toUpperCase().padStart(8, '0');
  const ms = Math.round(performance.now() - t1);
  if (r !== 0) {
    failed++;
    console.log(`${name}: failed (${r})`);
  } else {
    console.log(`${name}: ${fs.statSync(file).size} bytes, CRC-32 ${crc}, ${ms} ms`);
    const p = m._dl_manifest_entry(i);
    entries.push(m.HEAPU8.slice(p, p + 32));
  }
}
if (firstArg === undefined && failed === 0) {
  const all = new Uint8Array(entries.length * 32);
  entries.forEach((e, i) => all.set(e, i * 32));
  const buf = m._malloc(all.length);
  m.HEAPU8.set(all, buf);
  const h = m._dl_manifest_header(buf, entries.length, bits);
  const header = m.HEAPU8.slice(h, h + 32);
  m._free(buf);
  fs.writeFileSync(path.join(outDir, 'SET.DLM'), Buffer.concat([header, all]));
  console.log(`SET.DLM: ${entries.length} clips`);
}
console.log(`${count} clips, ${failed} failed, ${((performance.now() - t0) / 1000).toFixed(1)} s`);
process.exit(failed ? 1 : 0);
