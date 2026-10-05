// The page end to end in headless Chrome (Puppeteer): served from
// web/dist/site, given a CD-ROM image, a set converted into DLAIR.zip, the
// zip saved into a folder. Run in the Puppeteer image:
//
//   docker run --rm -v $PWD:/src -v IMAGE:/iso/DL.ISO:ro -v OUT:/out \
//     -e NODE_PATH=/home/pptruser/node_modules ghcr.io/puppeteer/puppeteer \
//     node /src/web/test/page.cjs /iso/DL.ISO ste|st|both /out

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const puppeteer = require('puppeteer');

const [image, set, out] = process.argv.slice(2);
const SITE = path.join(__dirname, '..', 'dist', 'site');
const TYPES = {
  '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript',
  '.mjs': 'text/javascript', '.wasm': 'application/wasm',
};

const server = http.createServer((req, res) => {
  const file = path.join(SITE, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(SITE) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    const index = path.join(file, 'index.html');
    if (fs.existsSync(index)) {
      res.writeHead(200, { 'Content-Type': 'text/html' });
      fs.createReadStream(index).pipe(res);
      return;
    }
    res.writeHead(404);
    res.end();
    return;
  }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});

(async () => {
  await new Promise((r) => server.listen(8000, r));
  const browser = await puppeteer.launch({ args: ['--no-sandbox'] });
  const page = await browser.newPage();
  page.on('console', (m) => console.log(`page: ${m.text()}`));
  page.on('pageerror', (e) => console.log(`page error: ${e.message}`));
  const requests = [];
  page.on('request', (r) => requests.push(r.url()));
  await page.goto(`http://localhost:8000/?set=${set}`);
  const t0 = Date.now();
  const input = await page.$('#image');
  await input.uploadFile(image);
  await page.waitForFunction(() => /Ready|not/.test(document.getElementById('image-status').textContent), { timeout: 120000 });
  console.log(await page.$eval('#image-status', (e) => e.textContent));
  await page.evaluate(() => {
    const zip = document.querySelector('input[name="where"][value="zip"]');
    if (zip) zip.checked = true;
  });
  await page.click('#go');
  let last = '';
  while (!(await page.$eval('#done', (e) => !e.hidden))) {
    const now = await page.$eval('#count', (e) => e.textContent) + ', ' +
      await page.$eval('#left', (e) => e.textContent);
    if (now !== last) {
      last = now;
      console.log(`${((Date.now() - t0) / 1000).toFixed(0)} s: ${now}`);
    }
    const error = await page.$eval('#error', (e) => (e.hidden ? '' : e.textContent));
    if (error) throw new Error(error);
    await new Promise((r) => setTimeout(r, 5000));
  }
  console.log(await page.$eval('#done-text', (e) => e.textContent));
  const client = await page.createCDPSession();
  await client.send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: out });
  await page.click('#done-link a');
  const zip = path.join(out, 'DLAIR.zip');
  for (let i = 0; i < 600 && !(fs.existsSync(zip) && !fs.existsSync(`${zip}.crdownload`)); i++) {
    await new Promise((r) => setTimeout(r, 1000));
  }
  await new Promise((r) => setTimeout(r, 2000));
  console.log(`${zip}: ${fs.statSync(zip).size} bytes`);
  const elsewhere = requests.filter((u) => !u.startsWith('http://localhost:8000/') && !u.startsWith('blob:'));
  console.log(`requests outside the page's own files: ${elsewhere.length}`);
  await browser.close();
  server.close();
})().catch((err) => {
  console.error(err);
  process.exit(1);
});
