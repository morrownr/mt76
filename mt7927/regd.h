/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2025 MediaTek Inc. */

#ifndef __MT7927_REGD_H
#define __MT7927_REGD_H

#include "mt7927.h"

int mt7927_mcu_regd_update(struct mt792x_dev *dev, u8 *alpha2,
			   enum environment_cap country_ie_env);

void mt7927_regd_be_ctrl(struct mt792x_dev *dev, u8 *alpha2);
void mt7927_regd_notifier(struct wiphy *wiphy, struct regulatory_request *req);
bool mt7927_regd_clc_supported(struct mt792x_dev *dev);
int mt7927_regd_change(struct mt792x_phy *phy, char *alpha2);
int mt7927_regd_init(struct mt792x_phy *phy);

#endif

