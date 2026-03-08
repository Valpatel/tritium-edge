import { test, expect } from '@playwright/test';

test.describe('Dashboard View', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
    // Dashboard is the default active view
  });

  test('fleet stats container is present', async ({ page }) => {
    const statsContainer = page.locator('#fleet-stats');
    await expect(statsContainer).toBeVisible();
  });

  test('fleet stats cards render after data loads', async ({ page }) => {
    // Wait for the stat cards to be populated (JS fetches /api/fleet/stats)
    const statCards = page.locator('#fleet-stats .stat-card');
    await expect(statCards.first()).toBeVisible({ timeout: 10000 });

    // Expect standard fleet stat cards: Total Nodes, Online, Offline, BLE Devices, Server Uptime
    await expect(statCards).toHaveCount(5);
  });

  test('fleet stats display expected labels', async ({ page }) => {
    await page.locator('#fleet-stats .stat-card').first().waitFor({ timeout: 10000 });

    const labels = page.locator('#fleet-stats .stat-card .label');
    const labelTexts = await labels.allTextContents();
    expect(labelTexts).toContain('Total Nodes');
    expect(labelTexts).toContain('Online');
    expect(labelTexts).toContain('Offline');
  });

  test('device grid is present', async ({ page }) => {
    const deviceGrid = page.locator('#device-grid');
    await expect(deviceGrid).toBeVisible();
  });

  test('recent events section is present', async ({ page }) => {
    const events = page.locator('#dash-events');
    await expect(events).toBeVisible();
  });

  test('navigating to devices view shows device table', async ({ page }) => {
    await page.locator('.nav-item[data-view="devices"]').click();

    const deviceTable = page.locator('#device-table');
    await expect(deviceTable).toBeVisible();

    // Verify table headers
    const headers = deviceTable.locator('th');
    const headerTexts = await headers.allTextContents();
    expect(headerTexts.some(h => h.includes('Device ID'))).toBeTruthy();
    expect(headerTexts.some(h => h.includes('Status'))).toBeTruthy();
    expect(headerTexts.some(h => h.includes('Firmware'))).toBeTruthy();
  });

  test('device table columns are sortable', async ({ page }) => {
    await page.locator('.nav-item[data-view="devices"]').click();

    const sortableHeaders = page.locator('#device-table th[data-sort]');
    const count = await sortableHeaders.count();
    expect(count).toBeGreaterThan(0);
  });
});
