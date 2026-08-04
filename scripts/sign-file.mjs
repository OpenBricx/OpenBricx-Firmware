// Sign a single file with the OpenBricx publisher key, writing a detached
// `<file>.sig` (raw 64-byte Ed25519). Used for firmware.json.
//
//   node scripts/sign-file.mjs <file> [--key <private.pem>]
//
// Same key as the Console's plugin pipeline — resolution order:
//   1. --key <path>
//   2. $OBX_PLUGIN_PRIVATE_KEY (base64 PKCS8 — for CI)
//   3. .plugin-keys/private.pem (local default)
// The matching PUBLIC key is embedded in the Console
// (src-tauri/src/plugins_host.rs TRUSTED_KEYS), which verifies both the plugin
// catalog and this firmware catalog against it.

import { createPrivateKey, sign } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(fileURLToPath(new URL('.', import.meta.url)), '..');

function loadPrivateKey(keyPath = null) {
  if (keyPath) return createPrivateKey(readFileSync(keyPath));
  if (process.env.OBX_PLUGIN_PRIVATE_KEY) {
    const der = Buffer.from(process.env.OBX_PLUGIN_PRIVATE_KEY.trim(), 'base64');
    return createPrivateKey({ key: der, format: 'der', type: 'pkcs8' });
  }
  const def = join(repoRoot, '.plugin-keys', 'private.pem');
  if (existsSync(def)) return createPrivateKey(readFileSync(def));
  throw new Error(
    'No private key. Pass --key <pem>, set OBX_PLUGIN_PRIVATE_KEY, or place .plugin-keys/private.pem.',
  );
}

const args = process.argv.slice(2);
const keyFlag = args.indexOf('--key');
const keyPath = keyFlag !== -1 ? args[keyFlag + 1] : null;
const file = args.find((a, i) => !a.startsWith('--') && (keyFlag === -1 || i !== keyFlag + 1));

if (!file) {
  console.error('usage: node scripts/sign-file.mjs <file> [--key <private.pem>]');
  process.exit(1);
}

const bytes = readFileSync(file);
const signature = sign(null, bytes, loadPrivateKey(keyPath));
writeFileSync(`${file}.sig`, signature);
console.log(`Signed ${file} -> ${file}.sig (${signature.length} bytes)`);
