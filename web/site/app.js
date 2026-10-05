// The page: the user's CD-ROM image checked by a worker, then every clip of
// the sets chosen converted by a pool of workers (convert-worker.js), each
// clip file written to the card's folder (a browser that can) or into
// DLAIR.zip (zip-worker.js), each set's manifest last. Nothing leaves the
// computer.

const $ = (id) => document.getElementById(id);
const canPickFolder = 'showDirectoryPicker' in window;
const WORKERS = Math.max(1, Math.min(8, (navigator.hardwareConcurrency || 4) - 1));
const SET_NAMES = { 4: 'STE', 3: 'ST' };

let image = null;  // the File
let clips = [];    // {name, size}, in the image's order
let running = false;
let workers = [];

// --- Workers --------------------------------------------------------------------

// The worker's next message of one of `types`.
function next(worker, types) {
  return new Promise((resolve) => {
    worker.onmessage = (event) => {
      if (types.includes(event.data.type)) resolve(event.data);
    };
  });
}

function request(worker, message, types) {
  const reply = next(worker, types);
  worker.postMessage(message);
  return reply;
}

function convertWorker() {
  return new Worker('convert-worker.js', { type: 'module' });
}

// --- Step 1: the image --------------------------------------------------------

function status(text, kind) {
  const el = $('image-status');
  el.textContent = text;
  el.className = `status ${kind || ''}`;
}

async function chooseImage(file) {
  image = null;
  clips = [];
  updateGo();
  status(`Reading ${file.name}…`);
  const worker = convertWorker();
  const opened = await request(worker, { type: 'open', file, list: true }, ['opened']);
  worker.terminate();
  if (opened.error !== undefined) {
    status(`${file.name} is not a CD-ROM image (${opened.error}).`, 'bad');
    return;
  }
  if (!opened.clips.some((c) => c.name === 'S01.MPG')) {
    status(`${file.name} is not the game's CD-ROM: it has no S01.MPG.`, 'bad');
    return;
  }
  image = file;
  clips = opened.clips;
  $('version').textContent = opened.version;
  const mb = clips.reduce((a, c) => a + c.size, 0) / 1e6;
  status(`${file.name}: ${clips.length} clips, ${mb.toFixed(0)} MB of video. Ready.`, 'good');
  updateGo();
}

function updateGo() {
  $('go').disabled = running || clips.length === 0 ||
    !($('set-ste').checked || $('set-st').checked);
}

// --- Where the files go -------------------------------------------------------
//
// A sink takes the files one at a time, in order (`add` waits for the one
// before), which also holds a worker back while the card is slower than it.

async function folderSink(sets) {
  let root = await window.showDirectoryPicker({ id: 'dlair', mode: 'readwrite' });
  if (root.name.toUpperCase() !== 'DLAIR') {
    root = await root.getDirectoryHandle('DLAIR', { create: true });
  }
  const dirs = {};
  for (const bits of sets) {
    dirs[bits] = await root.getDirectoryHandle(SET_NAMES[bits], { create: true });
    try {
      await dirs[bits].removeEntry('SET.DLM');  // an older set's: rewritten last
    } catch (err) {
      // none there
    }
  }
  let chain = Promise.resolve();
  return {
    add: (bits, name, bytes) => (chain = chain.then(async () => {
      const file = await dirs[bits].getFileHandle(name, { create: true });
      const writable = await file.createWritable();
      await writable.write(bytes);
      await writable.close();
    })),
    close: async () => {
      await chain;
      return null;
    },
  };
}

async function zipSink() {
  const zip = new Worker('zip-worker.js', { type: 'module' });
  const opened = await request(zip, { type: 'open' }, ['opened', 'error']);
  if (opened.type === 'error') throw new Error(opened.message);
  let chain = Promise.resolve();
  return {
    add: (bits, name, bytes) => (chain = chain.then(async () => {
      const reply = next(zip, ['added', 'error']);
      zip.postMessage({ type: 'add', name: `DLAIR/${SET_NAMES[bits]}/${name}`, bytes }, [bytes]);
      const added = await reply;
      if (added.type === 'error') throw new Error(added.message);
    })),
    close: async () => {
      await chain;
      const closed = await request(zip, { type: 'close' }, ['closed', 'error']);
      zip.terminate();
      if (closed.type === 'error') throw new Error(closed.message);
      return closed.file;
    },
  };
}

// --- The conversion -----------------------------------------------------------

function concat(parts) {
  const all = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
  let at = 0;
  for (const p of parts) {
    all.set(p, at);
    at += p.length;
  }
  return all;
}

function minutes(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  return s < 60 ? `${s} s` : `${Math.floor(s / 60)} min ${s % 60} s`;
}

function showError(text) {
  const el = $('error');
  el.textContent = text;
  el.hidden = !text;
}

async function convert() {
  const sets = [];
  if ($('set-ste').checked) sets.push(4);
  if ($('set-st').checked) sets.push(3);
  const toFolder = canPickFolder &&
    document.querySelector('input[name="where"]:checked').value === 'folder';
  showError('');
  let sink;
  try {
    sink = toFolder ? await folderSink(sets) : await zipSink();
  } catch (err) {
    if (err.name !== 'AbortError') showError(`The files cannot be written: ${err.message}`);
    return;
  }
  running = true;
  updateGo();
  $('done').hidden = true;
  $('progress').hidden = false;
  $('progress-title').textContent = 'Converting';
  $('progress').scrollIntoView({ behavior: 'smooth', block: 'start' });  // below the fold

  // The jobs: every clip of every set, the largest first so that the
  // workers end together.
  const jobs = [];
  for (const bits of sets) {
    clips.forEach((c, index) => jobs.push({ bits, index, name: c.name, size: c.size }));
  }
  const queue = [...jobs].sort((a, b) => b.size - a.size);
  const total = jobs.reduce((a, j) => a + j.size, 0);
  const entries = { 3: [], 4: [] };
  const partial = new Map();
  const failures = [];
  let doneBytes = 0;
  let finished = 0;
  let writing = 0;  // converted, waiting their turn to be written
  const t0 = performance.now();

  const count = Math.min(WORKERS, queue.length);
  workers = Array.from({ length: count }, convertWorker);
  await Promise.all(workers.map((w) => request(w, { type: 'open', file: image, list: false }, ['opened'])));

  const list = $('clips');
  list.textContent = '';
  const lines = workers.map(() => {
    const li = document.createElement('li');
    li.innerHTML = '<span class="name"></span><div class="bar"><div class="fill"></div></div>';
    list.appendChild(li);
    return li;
  });

  let drawn = false;
  const render = () => {
    if (drawn) return;
    drawn = true;
    requestAnimationFrame(() => {
      drawn = false;
      let bytes = doneBytes;
      for (const v of partial.values()) bytes += v;
      const fraction = total ? bytes / total : 0;
      $('overall').style.width = `${(fraction * 100).toFixed(1)}%`;
      $('count').textContent = `${finished} of ${jobs.length} clips` +
        (writing ? `, ${writing} being written` : '');
      const elapsed = performance.now() - t0;
      $('left').textContent = fraction > 0.03
        ? `about ${minutes(elapsed / fraction - elapsed)} left`
        : 'estimating the time left…';
    });
  };
  render();

  const work = async (worker, line) => {
    while (running && queue.length > 0) {
      const job = queue.shift();
      line.querySelector('.name').textContent = `${job.name} ${SET_NAMES[job.bits]}`;
      const fill = line.querySelector('.fill');
      fill.style.width = '0';
      const result = await new Promise((resolve) => {
        worker.onmessage = (event) => {
          const msg = event.data;
          if (msg.type === 'progress') {
            fill.style.width = `${msg.permille / 10}%`;
            partial.set(job, (job.size * msg.permille) / 1000);
            render();
          } else if (msg.type === 'done' || msg.type === 'failed') {
            resolve(msg);
          }
        };
        worker.postMessage({ type: 'convert', job: 0, index: job.index, bits: job.bits });
      });
      partial.delete(job);
      if (result.type === 'done') {
        // Converted: counted at once. Its file then waits its turn (a card
        // can be slower than the workers), the worker with it, its bar
        // full and dimmed meanwhile.
        doneBytes += job.size;
        finished++;
        writing++;
        fill.style.width = '100%';
        line.classList.add('writing');
        render();
        await sink.add(job.bits, job.name.replace(/\.MPG$/i, '.DLC'), result.bytes);
        writing--;
        line.classList.remove('writing');
        entries[job.bits][job.index] = result.entry;
      } else {
        failures.push(`${job.name} ${SET_NAMES[job.bits]} (${result.error})`);
      }
      render();
    }
    line.querySelector('.name').textContent = '';
    line.querySelector('.fill').style.width = '0';
  };

  try {
    await Promise.all(workers.map((w, i) => work(w, lines[i])));
    if (!running) return;  // stopped
    $('progress-title').textContent = 'Writing the last files';
    // Each complete set's manifest, last.
    const complete = [];
    for (const bits of sets) {
      const e = entries[bits];
      if (e.length === clips.length && e.every(Boolean)) {
        const all = concat(e);
        const reply = await request(workers[0], { type: 'header', entries: all, count: clips.length, bits }, ['header']);
        await sink.add(bits, 'SET.DLM', concat([reply.header, all]).buffer);
        complete.push(SET_NAMES[bits]);
      }
    }
    const file = await sink.close();
    finish(complete, failures, file, performance.now() - t0, toFolder);
  } catch (err) {
    showError(`The conversion stopped: ${err.message}`);
    stop();
  }
}

function finish(complete, failures, file, ms, toFolder) {
  running = false;
  workers.forEach((w) => w.terminate());
  workers = [];
  updateGo();
  $('progress').hidden = true;
  $('done').hidden = false;
  $('done-title').textContent = failures.length === 0 ? 'Done' : 'Done, with clips that failed';
  const sets = complete.length ? `The ${complete.join(' and ')} set${complete.length > 1 ? 's are' : ' is'} ready` : 'No set is complete';
  $('done-text').textContent = `${sets}, in ${minutes(ms)}.` +
    (failures.length ? ` Failed: ${failures.join(', ')}.` : '');
  const link = $('done-link');
  link.textContent = '';
  if (file) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(file);
    a.download = 'DLAIR.zip';
    a.className = 'button';
    a.textContent = `Save DLAIR.zip (${(file.size / 1e6).toFixed(0)} MB)`;
    link.appendChild(a);
    const p = document.createElement('p');
    p.textContent = 'Unzip it at the top of the card: it holds the DLAIR folder.';
    link.appendChild(p);
  } else if (toFolder) {
    link.textContent = 'The files are on the card, in its DLAIR folder.';
  }
}

function stop() {
  running = false;
  workers.forEach((w) => w.terminate());
  workers = [];
  updateGo();
  $('progress-title').textContent = 'Stopped';
  $('clips').textContent = '';
}

// --- The page ------------------------------------------------------------------

const want = (new URLSearchParams(location.search).get('set') || '').toLowerCase();
if (want === 'st') {
  $('set-st').checked = true;
  $('set-ste').checked = false;
} else if (want === 'both') {
  $('set-st').checked = true;
}
$(canPickFolder ? 'where-folder' : 'where-zip').hidden = false;

$('image').addEventListener('change', (e) => {
  if (e.target.files[0]) chooseImage(e.target.files[0]);
});
const drop = $('drop');
drop.addEventListener('dragover', (e) => {
  e.preventDefault();
  drop.classList.add('over');
});
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  if (e.dataTransfer.files[0]) chooseImage(e.dataTransfer.files[0]);
});
$('set-ste').addEventListener('change', updateGo);
$('set-st').addEventListener('change', updateGo);
$('go').addEventListener('click', convert);
$('stop').addEventListener('click', stop);
window.addEventListener('beforeunload', (e) => {
  if (running) e.preventDefault();
});
