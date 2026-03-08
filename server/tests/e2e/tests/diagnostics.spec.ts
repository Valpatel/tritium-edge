import { test, expect } from '@playwright/test';

test.describe('Diagnostics View', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
    await page.locator('.nav-item[data-view="diagnostics"]').click();
  });

  test('diagnostics view becomes active', async ({ page }) => {
    const view = page.locator('#view-diagnostics');
    await expect(view).toHaveClass(/active/);
  });

  test('diagnostics stats container is present', async ({ page }) => {
    const stats = page.locator('#diag-stats');
    await expect(stats).toBeVisible();
  });

  test('diagnostics stats load with expected labels', async ({ page }) => {
    // Wait for stat cards to populate from /api/fleet/diagnostics + /api/fleet/anomalies
    const statCards = page.locator('#diag-stats .stat-card');
    await expect(statCards.first()).toBeVisible({ timeout: 10000 });

    const labels = page.locator('#diag-stats .stat-card .label');
    const labelTexts = await labels.allTextContents();
    expect(labelTexts).toContain('Nodes Reporting');
    expect(labelTexts).toContain('Total Anomalies');
  });

  test('critical nodes section is present', async ({ page }) => {
    const criticalSection = page.locator('#diag-critical');
    await expect(criticalSection).toBeVisible();
  });

  test('anomaly table has correct headers', async ({ page }) => {
    const anomalyHeaders = page.locator('#view-diagnostics .data-table').first().locator('th');
    const headerTexts = await anomalyHeaders.allTextContents();
    expect(headerTexts).toContain('Node');
    expect(headerTexts).toContain('Subsystem');
    expect(headerTexts).toContain('Description');
    expect(headerTexts).toContain('Severity');
  });

  test('anomaly table body is present', async ({ page }) => {
    const tbody = page.locator('#diag-anomaly-tbody');
    await expect(tbody).toBeAttached();
  });

  test('node health table has correct headers', async ({ page }) => {
    // The second data-table in diagnostics is the health table
    const healthHeaders = page.locator('#view-diagnostics .data-table').nth(1).locator('th');
    const headerTexts = await healthHeaders.allTextContents();
    expect(headerTexts).toContain('Node');
    expect(headerTexts).toContain('Heap');
    expect(headerTexts).toContain('CPU Temp');
    expect(headerTexts).toContain('RSSI');
    expect(headerTexts).toContain('Reboots');
    expect(headerTexts).toContain('Anomalies');
  });

  test('node health table body is present', async ({ page }) => {
    const tbody = page.locator('#diag-node-tbody');
    await expect(tbody).toBeAttached();
  });
});
