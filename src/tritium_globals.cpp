/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Global instances for tritium_compat.h and tritium_i2c.h
 */
#include "tritium_compat.h"
#include "tritium_i2c.h"

#ifndef SIMULATOR
SerialPort Serial;
TritiumI2C i2c0;
#endif
