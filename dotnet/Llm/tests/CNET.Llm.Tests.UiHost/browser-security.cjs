// Requires an owner-installed playwright-core and Chromium; never downloads dependencies or models.
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const path = require('node:path');
const readline = require('node:readline');
const { chromium } = require(process.env.CNET_TEST_PLAYWRIGHT_MODULE || 'playwright-core');

(async () => {
  const host = spawn('dotnet', ['run', '--no-build', '-c', 'Release', '--project', path.join(__dirname, 'CNET.Llm.Tests.UiHost.csproj')],
    { stdio: ['pipe', 'pipe', 'pipe'] });
  let browser;
  try {
    const address = await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('Private UI fixture did not become ready')), 15000);
      host.once('exit', () => { clearTimeout(timeout); reject(new Error('Private UI fixture exited before readiness')); });
      readline.createInterface({ input: host.stdout }).on('line', line => {
        if (line.startsWith('UI_FIXTURE_READY ')) { clearTimeout(timeout); resolve(JSON.parse(line.slice(17)).address); }
      });
      host.stderr.on('data', () => {}); // Do not copy arbitrary host diagnostics into browser output.
    });
    assert.equal(new URL(address).hostname, '127.0.0.1');
    assert.equal(new URL(address).protocol, 'https:');
    browser = await chromium.launch({ headless: true });
    const context = await browser.newContext({ ignoreHTTPSErrors: true }); // Ephemeral local test certificate only.
    const external = [];
    await context.route('**/*', route => {
      if (new URL(route.request().url()).origin !== address) { external.push(route.request().url()); return route.abort(); }
      return route.continue();
    });
    const page = await context.newPage();
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(error.name));
    const requests = [];
    page.on('request', request => requests.push({ url: request.url(), method: request.method(), auth: request.headers().authorization }));
    const response = await page.goto(address);
    assert.equal(response.status(), 200, 'MANAGED_UI_RED anonymous_static_bootstrap');
    await page.getByLabel('Inference key').fill('fixture-inference-key-not-secret-0001');
    await page.getByRole('button', { name: 'Use inference key', exact: true }).click();
    assert.equal(await page.getByLabel('Inference key').inputValue(), '');
    await page.getByLabel('Message', { exact: true }).fill('Private fixture prompt');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await page.getByRole('status').filter({ hasText: 'Reply complete' }).waitFor();
    assert.ok((await page.locator('.message.assistant .message-text').textContent()).includes('<img'));
    assert.equal(await page.locator('.message.assistant img, .message.assistant a, .message.assistant script').count(), 0);
    assert.equal(await page.evaluate(() => window.fixtureXss), undefined);
    assert.equal(await page.evaluate(() => localStorage.length + sessionStorage.length), 0);
    assert.equal((await context.cookies()).length, 0);
    const inference = requests.filter(request => request.url.endsWith('/v1/chat/completions'));
    assert.equal(inference.length, 1);
    assert.equal(inference[0].auth, 'Bearer fixture-inference-key-not-secret-0001');
    assert.ok(requests.every(request => !request.url.includes('fixture-inference-key')));
    assert.ok(requests.filter(request => request.method === 'POST').every(request => request.url.endsWith('/v1/chat/completions')));
    const denied = await page.evaluate(async () => (await fetch('/v1/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' })).status);
    assert.equal(denied, 401);

    // Loading/stop resets controls and does not label incomplete output successful.
    await page.getByLabel('Message', { exact: true }).fill('Cancel this fixture request');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await page.getByRole('status').filter({ hasText: 'Stopped' }).waitFor();
    assert.equal(await page.getByRole('button', { name: 'Send', exact: true }).isEnabled(), true);

    // Semantic labels, native keyboard controls, viewport overflow and contrast-friendly layout.
    for (const width of [320, 768, 1024, 1440]) {
      await page.setViewportSize({ width, height: 900 });
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true, `overflow at ${width}`);
      if (width === 320 && process.env.CNET_UI_SCREENSHOT) await page.screenshot({ path: process.env.CNET_UI_SCREENSHOT + '.mobile.png', fullPage: true });
    }
    await page.getByLabel('Message', { exact: true }).focus();
    await page.keyboard.press('Tab');
    assert.equal(await page.getByRole('button', { name: 'Send', exact: true }).evaluate(element => element === document.activeElement), true);
    const contrast = await page.evaluate(() => {
      const luminance = color => {
        const rgb = color.match(/[\d.]+/g).slice(0, 3).map(value => {
          const c = Number(value) / 255;
          return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
        });
        return rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722;
      };
      const foreground = luminance(getComputedStyle(document.getElementById('key-help')).color);
      const background = luminance(getComputedStyle(document.querySelector('.credentials')).backgroundColor);
      return (Math.max(foreground, background) + 0.05) / (Math.min(foreground, background) + 0.05);
    });
    assert.ok(contrast >= 4.5, 'Muted helper text contrast');
    if (process.env.CNET_UI_SCREENSHOT) await page.screenshot({ path: process.env.CNET_UI_SCREENSHOT, fullPage: true });
    // Hold the completion cleanup boundary so Forget cannot be raced by a stale success continuation.
    await page.getByRole('button', { name: 'New chat', exact: true }).click();
    await page.evaluate(() => {
      const original = ReadableStreamDefaultReader.prototype.cancel;
      ReadableStreamDefaultReader.prototype.cancel = function () {
        window.fixtureCleanupEntered = true;
        return new Promise(resolve => {
          window.fixtureReleaseCleanup = () => { ReadableStreamDefaultReader.prototype.cancel = original; resolve(); };
        });
      };
    });
    await page.getByLabel('Message', { exact: true }).fill('Forget this race fixture');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await page.waitForFunction(() => window.fixtureCleanupEntered === true);
    await page.getByRole('button', { name: 'Forget key', exact: true }).click();
    await page.evaluate(() => window.fixtureReleaseCleanup());
    await page.getByRole('button', { name: 'New chat', exact: true }).waitFor({ state: 'visible' });
    await page.waitForFunction(() => !document.getElementById('new-chat').disabled);
    assert.equal(await page.getByRole('status').textContent(), 'Key and conversation forgotten.', 'MANAGED_UI_RED forgotten_session_revived');
    assert.equal(await page.locator('.message').count(), 0);
    await page.reload();
    assert.equal(await page.getByLabel('Inference key').inputValue(), '');
    assert.equal(await page.getByRole('button', { name: 'Send', exact: true }).isEnabled(), false);
    assert.deepEqual(external, []);
    assert.deepEqual(pageErrors, []);
    console.log('MANAGED_UI_BROWSER_GREEN protected_https_chat=1 text_only=1 ephemeral_key=1 no_admin=1 stop=1 viewports=4 keyboard=1 external_requests=0 forgotten_session_retained=1');
  } finally {
    if (browser) await browser.close();
    if (host.exitCode === null) {
      host.stdin.end('\n');
      await Promise.race([once(host, 'exit'), new Promise(resolve => setTimeout(resolve, 5000))]);
      if (host.exitCode === null) host.kill('SIGTERM');
    }
  }
})().catch(error => { console.error(error.message); process.exitCode = 1; });
