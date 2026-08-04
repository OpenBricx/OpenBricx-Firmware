// Build a signed-ready firmware.json from firmware-manifest.json.
//
//   node scripts/build-firmware-catalog.mjs --base-url <url> --out <dir>
//
// firmware-manifest.json (repo root) is the hand-maintained source of truth: each
// entry names a product/chip/version/kind and a LOCAL bin path. This script:
//   1. reads every listed bin, computing its sha256 + size,
//   2. copies it into <out>/ under its stable asset name (the file's basename),
//   3. writes <out>/firmware.json with `url` = <base-url>/<basename>.
//
// Sign the result (node scripts/sign-file.mjs <out>/firmware.json) and upload the
// whole <out>/ directory to a GitHub Release. The OpenBricx Console fetches
// firmware.json, verifies the Ed25519 signature, and refuses any downloaded image
// whose bytes don't match the sha256 committed here.

import { createHash } from 'node:crypto';
import { copyFileSync, mkdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(fileURLToPath(new URL('.', import.meta.url)), '..');

const args = process.argv.slice(2);
function flag(name) {
  const i = args.indexOf(name);
  return i !== -1 ? args[i + 1] : null;
}

const baseUrl = (flag('--base-url') || '').replace(/\/$/, '');
const outDir = flag('--out') || 'release';
if (!baseUrl) {
  console.error('usage: node scripts/build-firmware-catalog.mjs --base-url <url> [--out <dir>]');
  process.exit(1);
}

const manifestPath = join(repoRoot, 'firmware-manifest.json');
const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
if (!Array.isArray(manifest.firmware)) {
  console.error(`${manifestPath} must have a "firmware" array`);
  process.exit(1);
}

mkdirSync(outDir, { recursive: true });
const sha256Hex = (buf) => createHash('sha256').update(buf).digest('hex');

const entries = manifest.firmware.map((e) => {
  for (const field of ['product', 'chip', 'version', 'kind', 'file']) {
    if (!e[field]) throw new Error(`entry missing "${field}": ${JSON.stringify(e)}`);
  }
  if (e.kind !== 'flash' && e.kind !== 'ota') {
    throw new Error(`entry kind must be "flash" or "ota": ${JSON.stringify(e)}`);
  }
  const src = resolve(repoRoot, e.file);
  const bytes = readFileSync(src);
  const name = basename(src);
  copyFileSync(src, join(outDir, name));
  return {
    product: e.product,
    chip: e.chip,
    hwRev: e.hwRev ?? '',
    version: e.version,
    kind: e.kind,
    url: `${baseUrl}/${name}`,
    sha256: sha256Hex(bytes),
    size: statSync(src).size,
    notes: e.notes ?? '',
  };
});

const catalog = { schema: 1, firmware: entries };
writeFileSync(join(outDir, 'firmware.json'), JSON.stringify(catalog, null, 2) + '\n');

console.log(`Wrote ${outDir}/firmware.json with ${entries.length} image(s):`);
for (const e of entries) {
  console.log(`  ${e.product} ${e.kind} v${e.version} (${e.chip}) -> ${e.url}`);
}
