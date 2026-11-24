// SPDX-License-Identifier: GPL-3.0-only
// Browser test of the web UI against a fresh QEMU device:
//
//   python test/qemu_e2e.py --build build-qemu --serve --listen 0.0.0.0   (in the IDF container)
//   node test/ui/ui_test.mjs --url http://127.0.0.1:18080 --out shots [--firmware build-qemu/leaf_inverter_bridge.bin]
//
// Runs in the mcr.microsoft.com/playwright:v1.65.1 image (see tools/ci_local.sh). Walks
// through the first-run flow a new owner sees: live dashboard, changing the inverter mode,
// the login and forced password change, the device and firmware pages and (with --firmware)
// a complete firmware update through the browser. Fails on console errors, uncaught page
// errors and Content-Security-Policy violations, and saves screenshots for the docs.
import { chromium } from 'playwright-core';
import fs from 'node:fs';
import path from 'node:path';

const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => {
  if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return acc;
}, []));
const URL = args.url || 'http://127.0.0.1:18080';
const OUT = args.out || 'ui-shots';
const NEW_PASSWORD = 'ui-Test-Pass-42';
fs.mkdirSync(OUT, { recursive: true });

let failures = 0;
function check(cond, what) {
  console.log(`  ${cond ? 'PASS' : 'FAIL'} ${what}`);
  if (!cond) failures++;
  return cond;
}

const problems = [];
function watch(page, name) {
  page.on('pageerror', (e) => problems.push(`${name}: page error: ${e.message}`));
  page.on('console', (msg) => {
    const text = msg.text();
    // 401/403 answers are part of the login flow under test.
    if (msg.type() === 'error' && !/status of 40[13]/.test(text)) problems.push(`${name}: console: ${text}`);
    if (/Content Security Policy/i.test(text)) problems.push(`${name}: CSP: ${text}`);
  });
}

async function waitLive(page) {
  await page.waitForFunction(() => document.querySelector('#conn-text')?.textContent === 'Live', null, { timeout: 30000 });
}

// Phone screenshots show the viewport (the bottom navigation is fixed); desktop ones the full page.
// The short wait lets CSS transitions (card highlight, bars) settle first.
async function shot(page, name) {
  await page.waitForTimeout(400);
  await page.screenshot({ path: path.join(OUT, `${name}.png`), fullPage: !name.startsWith('phone') });
}

const browser = await chromium.launch();
try {
  // ------------------------------------------------------------ phone, light
  console.log('[ui 1] phone: dashboard');
  const phone = await browser.newContext({
    viewport: { width: 412, height: 915 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true, colorScheme: 'light',
  });
  const page = await phone.newPage();
  watch(page, 'phone');
  await page.goto(URL);
  await waitLive(page);
  check(true, 'live WebSocket feed connected');
  check(await page.textContent('#gear') === 'P', 'gear shows P');
  check((await page.textContent('#mode-label')).includes('110 kW'), 'inverter mode shows 110 kW');
  check(await page.isVisible('#banner'), 'default-password banner is shown');
  check(await page.textContent('#bus-vcm .bus-state') === 'OK', 'VCM bus shown as OK');
  check(await page.textContent('#bus-inverter .bus-state') === 'Not running', 'inverter bus shown as not running');
  check(await page.isVisible('.tabs a[data-view="settings"]'), 'bottom navigation visible on a phone');
  await page.waitForTimeout(500);
  await shot(page, 'phone-dashboard');

  console.log('[ui 2] phone: change the inverter mode (login + forced password change)');
  await page.click('.tabs a[data-view="settings"]');
  await page.waitForSelector('#inverter-options .option', { state: 'visible' });
  check(await page.locator('#inverter-options .option').count() === 3, 'three inverter options');
  check(await page.locator('#other-settings select').count() === 6, 'six stored-only settings');
  await page.locator('#inverter-options .option', { hasText: '160 kW' }).click();
  check(await page.isVisible('#savebar'), 'save bar appears after a change');
  await shot(page, 'phone-settings');
  await page.click('#btn-save');
  await page.waitForSelector('#dlg-login[open]');
  check(true, 'saving asks for the admin password');
  await shot(page, 'phone-login');
  await page.fill('#login-password', 'Password');
  await page.click('#btn-login');
  await page.waitForSelector('#dlg-setpw[open]');
  check(await page.isHidden('#setpw-current'), 'forced password change knows the current password');
  await page.fill('#form-setpw input[name="new"]', NEW_PASSWORD);
  await page.fill('#form-setpw input[name="repeat"]', NEW_PASSWORD);
  await shot(page, 'phone-set-password');
  await page.click('#form-setpw button[type="submit"]');
  await page.waitForSelector('.toast >> text=Settings saved and applied', { timeout: 30000 });
  check(true, 'after the new password the pending save goes through');
  check(await page.isHidden('#savebar'), 'save bar disappears');
  check(await page.isHidden('#banner'), 'default-password banner disappears');
  await page.click('.tabs a[data-view="dashboard"]');
  await page.waitForFunction(() => document.querySelector('#mode-label').textContent.includes('160 kW'), null, { timeout: 10000 });
  check(true, 'dashboard shows the new 160 kW mode');

  console.log('[ui 3] phone: device and firmware pages');
  await page.click('.tabs a[data-view="device"]');
  await page.waitForFunction(() => document.querySelectorAll('#sys-info dt').length > 8, null, { timeout: 10000 });
  check(await page.textContent('#pw-badge') === 'Custom', 'password badge shows a custom password');
  check(await page.inputValue('#form-wifi input[name="ssid"]') === 'Can-bridgeINV', 'Wi-Fi form shows the SSID');
  await shot(page, 'phone-device');
  await page.click('.tabs a[data-view="firmware"]');
  await page.waitForFunction(() => document.querySelector('#fw-version').textContent !== '–', null, { timeout: 10000 });
  check(/^\d+\.\d+\.\d+/.test(await page.textContent('#fw-version')), 'installed firmware version shown');
  await shot(page, 'phone-firmware');

  if (args.firmware && args.firmware !== true) {
    console.log('[ui 4] phone: firmware update through the browser');
    await page.setInputFiles('#fw-file', args.firmware);
    check(await page.isEnabled('#btn-install'), 'install button enabled for a valid .bin');
    await page.click('#btn-install');
    await page.waitForFunction(() => /is running/.test(document.querySelector('#fw-status').textContent), null, { timeout: 300000 });
    check(true, `update installed, bridge restarted: "${await page.textContent('#fw-status')}"`);
    await shot(page, 'phone-firmware-updated');
  }
  await phone.close();

  // ------------------------------------------------------------ desktop, dark + light
  console.log('[ui 5] desktop screenshots');
  for (const scheme of ['dark', 'light']) {
    const desk = await browser.newContext({ viewport: { width: 1280, height: 860 }, colorScheme: scheme });
    const p = await desk.newPage();
    watch(p, `desktop-${scheme}`);
    await p.goto(URL);
    await waitLive(p);
    check(await p.isHidden('#banner'), `${scheme}: no default-password banner any more`);
    await p.waitForTimeout(600);
    await shot(p, `desktop-${scheme}-dashboard`);
    await p.click('.tabs a[data-view="settings"]');
    await p.waitForSelector('#inverter-options .option');
    await shot(p, `desktop-${scheme}-settings`);
    await p.click('.tabs a[data-view="device"]');
    await p.waitForFunction(() => document.querySelectorAll('#sys-info dt').length > 8, null, { timeout: 10000 });
    await shot(p, `desktop-${scheme}-device`);
    await desk.close();
  }
} catch (e) {
  check(false, `unexpected error: ${e.message}`);
} finally {
  await browser.close();
}

check(problems.length === 0, `no console errors, page errors or CSP violations${problems.length ? `:\n    ${problems.join('\n    ')}` : ''}`);
console.log(`\n${failures ? 'FAILED' : 'PASSED'}: ${failures} failure(s); screenshots in ${OUT}`);
process.exit(failures ? 1 : 0);
