// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE (GPL-3.0 with an additional permission
// to combine with the Meta Spatial SDK).
package com.xerx.questvisor

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable

@Composable
fun SpatialTheme(darkTheme: Boolean = true, content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (darkTheme) darkColorScheme() else lightColorScheme(),
        content = content,
    )
}

@Composable
fun SpatialSwitch(checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Switch(
        checked = checked,
        onCheckedChange = onCheckedChange,
    )
}
