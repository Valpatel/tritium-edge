import { test, expect } from '@playwright/test';

test.describe('Config Sync View', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
    await page.locator('.nav-item[data-view="config-sync"]').click();
  });

  test('config sync view becomes active', async ({ page }) => {
    const view = page.locator('#view-config-sync');
    await expect(view).toHaveClass(/active/);
  });

  test('config sync stats container is present', async ({ page }) => {
    const stats = page.locator('#config-sync-stats');
    await expect(stats).toBeVisible();
  });

  test('config sync stats load', async ({ page }) => {
    const statCards = page.locator('#config-sync-stats .stat-card');
    await expect(statCards.first()).toBeVisible({ timeout: 10000 });

    // Verify at least one stat card rendered
    const count = await statCards.count();
    expect(count).toBeGreaterThan(0);
  });

  test('drifted devices table has correct headers', async ({ page }) => {
    const headers = page.locator('#view-config-sync .data-table th');
    const headerTexts = await headers.allTextContents();
    expect(headerTexts).toContain('Device');
    expect(headerTexts).toContain('Drift Count');
    expect(headerTexts).toContain('Max Severity');
    expect(headerTexts).toContain('Details');
  });

  test('drifted devices table body is present', async ({ page }) => {
    const tbody = page.locator('#config-sync-tbody');
    await expect(tbody).toBeAttached();
  });

  test('section header shows Drifted Devices', async ({ page }) => {
    const header = page.locator('#view-config-sync .section-header');
    await expect(header).toContainText('Drifted Devices');
  });
});
