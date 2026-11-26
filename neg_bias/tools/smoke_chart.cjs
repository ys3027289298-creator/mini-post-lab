const { chromium } = require('/mnt/d/ImageReferenceRubricsJudge-v0.4.1/node_modules/playwright');

(async () => {
  const browser = await chromium.launch({
    executablePath: '/home/ys/.cache/ms-playwright/chromium-1255/chrome-linux64/chrome',
    args: ['--no-sandbox'],
  });
  const page = await browser.newPage({ viewport: { width: 1280, height: 1000 } });
  const errors = [];
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', e => errors.push(String(e)));

  const url = 'file://' + process.argv[2] + '/bias_curves.html';
  await page.goto(url);
  await page.waitForSelector('path.series');

  const series = await page.locator('path.series').count();
  const rows = await page.locator('#statTable tbody tr').count();
  const panels = await page.locator('.card').count();

  const card = page.locator('.card').first();
  const box = await card.boundingBox();
  await page.mouse.move(box.x + box.width * 0.55, box.y + box.height * 0.5);
  await page.waitForTimeout(100);
  const tipVisible = await card.locator('.tooltip').isVisible();
  const tipRows = await card.locator('.tooltip .tt-row').count();
  const tipTitle = await card.locator('.tooltip .tt-title').textContent();
  const dotsVisible = await card.locator('.hover-dot[style*="visible"]').count();

  console.log(JSON.stringify({
    panels, seriesPaths: series, tableRows: rows,
    tipVisible, tipRows, tipTitle, dotsVisible, consoleErrors: errors,
  }, null, 1));
  await browser.close();
  if (series !== 18 || rows !== 9 || !tipVisible || tipRows !== 3 || errors.length) process.exit(1);
})();
