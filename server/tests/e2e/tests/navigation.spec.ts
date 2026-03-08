import { test, expect } from '@playwright/test';

const NAV_ITEMS = [
  { view: 'dashboard', label: 'Dashboard' },
  { view: 'devices', label: 'Devices' },
  { view: 'ble', label: 'BLE Presence' },
  { view: 'firmware', label: 'Firmware' },
  { view: 'ota', label: 'OTA' },
  { view: 'map', label: 'Map' },
  { view: 'fleet-ota', label: 'Fleet OTA' },
  { view: 'provision', label: 'Provision' },
  { view: 'commands', label: 'Commands' },
  { view: 'diagnostics', label: 'Diagnostics' },
  { view: 'events', label: 'Events' },
  { view: 'config-sync', label: 'Config Sync' },
];

test.describe('Navigation', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
  });

  test('page loads with correct title', async ({ page }) => {
    await expect(page).toHaveTitle(/TRITIUM-EDGE/);
  });

  test('sidebar renders all navigation items', async ({ page }) => {
    for (const item of NAV_ITEMS) {
      const navItem = page.locator(`.nav-item[data-view="${item.view}"]`);
      await expect(navItem).toBeVisible();
      await expect(navItem).toContainText(item.label);
    }
  });

  test('dashboard view is active by default', async ({ page }) => {
    const dashNav = page.locator('.nav-item[data-view="dashboard"]');
    await expect(dashNav).toHaveClass(/active/);

    const dashView = page.locator('#view-dashboard');
    await expect(dashView).toHaveClass(/active/);
  });

  test('clicking nav item switches the active view', async ({ page }) => {
    for (const item of NAV_ITEMS) {
      const navItem = page.locator(`.nav-item[data-view="${item.view}"]`);
      await navItem.click();

      // Nav item should become active
      await expect(navItem).toHaveClass(/active/);

      // Corresponding view should be visible
      const view = page.locator(`#view-${item.view}`);
      await expect(view).toHaveClass(/active/);
    }
  });

  test('only one nav item is active at a time', async ({ page }) => {
    // Click Diagnostics
    await page.locator('.nav-item[data-view="diagnostics"]').click();

    const activeItems = page.locator('.nav-item.active');
    await expect(activeItems).toHaveCount(1);
    await expect(activeItems).toHaveAttribute('data-view', 'diagnostics');
  });

  test('only one view is visible at a time', async ({ page }) => {
    await page.locator('.nav-item[data-view="devices"]').click();

    // The active view should be devices
    const activeViews = page.locator('.view.active');
    await expect(activeViews).toHaveCount(1);

    // Dashboard view should not be active
    const dashView = page.locator('#view-dashboard');
    await expect(dashView).not.toHaveClass(/active/);
  });

  test('topbar logo is visible', async ({ page }) => {
    const logo = page.locator('.topbar .logo');
    await expect(logo).toBeVisible();
    await expect(logo).toContainText('TRITIUM-EDGE');
  });
});
