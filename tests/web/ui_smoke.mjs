// Browser smoke test for the web UI. Needs a running sloppy-synth and Playwright:
//   node tests/web/ui_smoke.mjs http://localhost:8080/ /tmp/screenshots
// (set PLAYWRIGHT_MODULE if playwright isn't resolvable from here). Assumes the
// library holds tests/fixtures/"Test Bank.vitalbank".
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const url = process.argv[2], out = process.argv[3];
const browser = await chromium.launch();
const errors = [];
async function page(viewport, name, actions) {
  const ctx = await browser.newContext({ viewport, deviceScaleFactor: 2, hasTouch: true });
  const p = await ctx.newPage();
  p.on('console', m => { if (m.type() === 'error') errors.push(`${name}: ${m.text()}`); });
  p.on('pageerror', e => errors.push(`${name}: ${e.message}`));
  await p.goto(url);
  await p.waitForSelector('#status.connected');
  await p.waitForSelector('.knob');
  await actions(p);
  await ctx.close();
}
await page({ width: 390, height: 844 }, 'phone', async (p) => {
  await p.screenshot({ path: `${out}/phone-play.png` });
  // play a note via keyboard tap
  const key = p.locator('.key.white').nth(2);
  const box = await key.boundingBox();
  await p.mouse.move(box.x + box.width / 2, box.y + box.height * 0.8);
  await p.mouse.down();
  await p.waitForTimeout(150);
  console.log('key down class:', await key.getAttribute('class'));
  await p.mouse.up();
  await p.click('.tabs button[data-view="patches"]');
  await p.waitForSelector('.patch-folder');
  // The library has one bank, so it opens straight into its folders.
  console.log('folders:', (await p.locator('.patch-folder .name').allTextContents()).join(', '));
  await p.locator('.patch-folder', { hasText: 'Pads' }).click();
  console.log('patches:', await p.locator('.patch-item').allTextContents());
  await p.screenshot({ path: `${out}/phone-patches.png` });
  await p.fill('#patch-search', 'bass');
  console.log('search "bass":', (await p.locator('.patch-item').allTextContents()).join(' | '));
  await p.fill('#patch-search', '');
  await p.locator('.patch-item', { hasText: 'Test Pad' }).click();
  await p.waitForFunction(() => document.getElementById('patch-name').textContent === 'Test Pad');
  console.log('loaded:', await p.textContent('#patch-name'), '|', await p.textContent('#patch-meta'));
  await p.click('.tabs button[data-view="edit"]');
  await p.locator('.chip', { hasText: 'Filter' }).click();
  await p.locator('.instance', { hasText: '2' }).click();
  await p.waitForSelector('.control');
  console.log('filter 2 controls:', (await p.locator('.control-label').allTextContents()).join(', '));
  const cutoff = p.locator('.control', { hasText: 'Cutoff' }).locator('input[type=range]');
  await cutoff.evaluate(el => { el.value = 100; el.dispatchEvent(new Event('input', { bubbles: true })); });
  await p.waitForTimeout(300);
  console.log('cutoff display:', await p.locator('.control', { hasText: 'Cutoff' }).locator('.control-value').textContent());
  await p.screenshot({ path: `${out}/phone-edit.png` });
  // Route LFO 4 to oscillator 3's level on the Mod page, check it shows up on
  // the LFO 4 page, then remove it.
  await p.locator('.chip', { hasText: 'Mod' }).click();
  await p.selectOption('select[aria-label="Modulation source"]', 'lfo_4');
  await p.selectOption('select[aria-label="Modulation destination"]', 'osc_3_level');
  await p.locator('.routes-add .pill', { hasText: 'Add' }).click();
  await p.waitForSelector('.routing:has-text("LFO 4")');
  await p.screenshot({ path: `${out}/phone-mod.png` });
  await p.locator('.chip', { hasText: 'LFO' }).click();
  await p.locator('.instance', { hasText: '4' }).click();
  console.log('LFO 4 modulates:', await p.locator('.routes .routing-title').allTextContents());
  await p.locator('.routes button[aria-label^="Remove"]').click();
  await p.waitForSelector('.routes-empty');
  console.log('after remove:', await p.locator('.routes-empty').textContent());
});
await page({ width: 1180, height: 820 }, 'tablet', async (p) => {
  await p.waitForTimeout(300);
  console.log('tablet patch now:', await p.textContent('#patch-name'));
  await p.screenshot({ path: `${out}/tablet-play.png` });
  await p.click('.tabs button[data-view="edit"]');
  await p.locator('.chip', { hasText: 'Filter' }).click();
  await p.locator('.instance', { hasText: '2' }).click();
  console.log('tablet sees cutoff:', await p.locator('.control', { hasText: 'Cutoff' }).locator('.control-value').textContent());
  await p.locator('.chip', { hasText: 'Osc' }).click();
  await p.screenshot({ path: `${out}/tablet-edit.png` });
});
await browser.close();
console.log('errors:', errors.length ? errors : 'none');
